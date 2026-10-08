// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef AMDF_SRC_GPU_UMD_SPARSE_MAPPING_TEST_H_
#define AMDF_SRC_GPU_UMD_SPARSE_MAPPING_TEST_H_

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ios>
#include <span>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "libamdf/cts/gpu/sdma/encoding/commands.h"

namespace amdf::testing {

// One physical slab, independent of its replaceable sparse aliases.
inline constexpr uint64_t kSparseSlabByteLength = 2 * 1024 * 1024;

// A discontiguous application range within one of two stable reservations.
struct SparseRange {
  // Index of the reservation containing this range.
  uint32_t reservation;
  // Byte offset from that reservation's base.
  uint64_t byte_offset;
  // Number of payload bytes, including small unaligned-to-page metadata.
  uint64_t byte_length;
};

// A whole-slab mapping whose backing identity survives replacement.
struct SparseMapping {
  // Index of the destination reservation.
  uint32_t reservation;
  // Slab-aligned byte offset within the reservation.
  uint64_t byte_offset;
  // Index of the retained native allocation providing this slab.
  uint32_t backing;
};

// Native-provider test seam, not a production or public VMM interface. Native
// failures terminate the test without claiming successful retirement. Healthy
// detach and release are checked explicitly before the queue is destroyed.
class SparseMappingBackend {
 public:
  virtual ~SparseMappingBackend() = default;

  // Prepares all native allocations, reservations, command storage and one
  // persistent queue. Nothing after this call creates a native allocation.
  virtual void Initialize(uint32_t backing_count,
                          uint64_t reservation_byte_length,
                          uint64_t payload_byte_length) = 0;
  // Establishes the requested aliases, replacing occupied destinations, and
  // waits for their native readiness. Prior users have already completed.
  virtual void Attach(std::span<const SparseMapping> mappings) = 0;
  // Removes aliases and checks native completion before any backing release.
  virtual void Detach(std::span<const SparseMapping> mappings) = 0;
  // Publishes a prepared SDMA stream to the existing queue and waits for its
  // execution-completion word, not merely consumption of ring storage.
  virtual void Execute(std::span<const uint32_t> words,
                       uint32_t completion_value) = 0;
  // After all users complete, clears entire reservations including holes,
  // checks detach completion, then releases backing before the queue/domain.
  virtual void Release() = 0;

  // Stable bases of the two sparse reservations.
  std::array<uint64_t, 2> reservation_addresses = {};
  // Original fixed aliases used to independently observe physical placement.
  std::vector<uint64_t> backing_addresses;
  // Host-writable source for prepared generation-tagged uploads.
  uint32_t* upload = nullptr;
  // Host-readable destination of exact readbacks.
  uint32_t* readback = nullptr;
  // Independent host-readable capture that replacement must not change.
  uint32_t* capture = nullptr;
  // GPU address of upload storage.
  uint64_t upload_address = 0;
  // GPU address of readback storage.
  uint64_t readback_address = 0;
  // GPU address of independent capture storage.
  uint64_t capture_address = 0;
  // Coherent GPU completion-word address, separate from all payload bytes.
  uint64_t completion_address = 0;
  // Encoding features queried from the actual SDMA queue family.
  amdf_queue_format_features_t format_features = 0;
  // Maximum stream size accepted by the actual queue, including completion.
  size_t command_byte_capacity = 0;
};

// Model-free sparse state and cache layout: 141 ranges, including a large state
// image, page-sized cache fragments, and 12/32-byte metadata. Both reservations
// extend above 4 GiB; offsets and command addresses must remain full-width.
inline std::array<SparseRange, 141> SparseRanges(uint32_t generation) {
  constexpr uint64_t kHigh = (uint64_t{1} << 32) + 512;
  constexpr uint64_t kStateBytes = 156893184;
  constexpr uint64_t kStateStride = kStateBytes + 256 + 2048 + 256;
  constexpr uint64_t kOrigin = kHigh + 7 * kStateStride;
  constexpr uint64_t kCache = kHigh + 17 * kStateStride;
  constexpr uint64_t kPlaneStride = 2048 * 2048;
  constexpr uint64_t kPageBytes = 131072;
  std::array<SparseRange, 141> ranges = {};
  ranges[0] = {0, kOrigin, 12};
  ranges[1] = {0, kHigh + (generation == 1 ? 7 : 5) * kStateStride + 256,
               kStateBytes};
  ranges[2] = {0, kOrigin + 256 + kStateBytes, 2048};
  ranges[3] = {0, kOrigin + 256 + kStateBytes + 2048, 32};
  ranges[4] = {1, kHigh, 20480};
  constexpr uint32_t kReplacementBlocks[] = {16, 2, 31, 10};
  for (uint32_t i = 0; i < 136; ++i) {
    const uint32_t block = generation == 1 ? i % 4 : kReplacementBlocks[i % 4];
    ranges[i + 5] = {
        i < 128 ? 0u : 1u,
        kCache + ((i / 4) % 32) * kPlaneStride + block * kPageBytes,
        kPageBytes};
  }
  return ranges;
}

inline std::vector<SparseMapping> SparseMappings(
    std::span<const SparseRange> ranges) {
  std::vector<SparseMapping> mappings;
  for (const auto& range : ranges) {
    const uint64_t end = range.byte_offset + range.byte_length;
    for (uint64_t offset = range.byte_offset & ~(kSparseSlabByteLength - 1);
         offset < end; offset += kSparseSlabByteLength) {
      mappings.push_back({range.reservation, offset, 0});
    }
  }
  std::sort(mappings.begin(), mappings.end(), [](const auto& a, const auto& b) {
    return a.reservation != b.reservation ? a.reservation < b.reservation
                                          : a.byte_offset < b.byte_offset;
  });
  mappings.erase(std::unique(mappings.begin(), mappings.end(),
                             [](const auto& a, const auto& b) {
                               return a.reservation == b.reservation &&
                                      a.byte_offset == b.byte_offset;
                             }),
                 mappings.end());
  return mappings;
}

enum class SparseTransfer { kUpload, kReadback, kCapture, kPhysicalReadback };

struct SparseCommandBatch {
  // Complete commands fitting one native queue publication.
  std::vector<uint32_t> words;
  // Execution-completion word written by this batch's last non-NOP command.
  uint32_t completion_value;
};

// Encodes the entire stream before the critical capture -> replace -> access
// interval. Transfers split at slab boundaries and the conservative SDMA copy
// length. The independent physical readback follows the retained source alias,
// so an upload and download using the same stale translation cannot self-mask.
inline std::vector<SparseCommandBatch> SparseTransferCommands(
    const SparseMappingBackend& backend, std::span<const SparseRange> ranges,
    std::span<const SparseMapping> mappings, SparseTransfer transfer,
    uint32_t completion_value) {
  const size_t word_capacity = backend.command_byte_capacity / sizeof(uint32_t);
  std::vector<SparseCommandBatch> batches;
  std::vector<uint32_t> words(word_capacity);
  SdmaCommandWriter writer(words.data(), backend.format_features);
  const auto finish_batch = [&]() {
    const uint32_t value =
        (completion_value << 16) | static_cast<uint32_t>(batches.size() + 1);
    writer.Fence32(backend.completion_address, value);
    while (writer.word_count() % 8 != 0) {
      writer.Noop();
    }
    words.resize(writer.word_count());
    batches.push_back({std::move(words), value});
    words.resize(word_capacity);
    writer = SdmaCommandWriter(words.data(), backend.format_features);
  };
  uint64_t payload_offset = 0;
  for (const auto& range : ranges) {
    for (uint64_t offset = 0; offset < range.byte_length;) {
      const uint64_t position = range.byte_offset + offset;
      const uint64_t slab_offset = position & (kSparseSlabByteLength - 1);
      const uint32_t byte_length = static_cast<uint32_t>(std::min(
          range.byte_length - offset, kSparseSlabByteLength - slab_offset));
      uint64_t sparse_address =
          backend.reservation_addresses[range.reservation] + position;
      if (transfer == SparseTransfer::kPhysicalReadback) {
        const auto mapping = std::find_if(
            mappings.begin(), mappings.end(), [&](const auto& entry) {
              return entry.reservation == range.reservation &&
                     entry.byte_offset == position - slab_offset;
            });
        sparse_address =
            backend.backing_addresses[mapping->backing] + slab_offset;
      }
      const uint64_t host_address =
          (transfer == SparseTransfer::kUpload    ? backend.upload_address
           : transfer == SparseTransfer::kCapture ? backend.capture_address
                                                  : backend.readback_address) +
          payload_offset + offset;
      // Reserve the copy, its completion packet, and final alignment padding.
      if (writer.word_count() + 7 + 4 + 7 > word_capacity) {
        finish_batch();
      }
      if (transfer == SparseTransfer::kUpload) {
        writer.CopyLinear(host_address, sparse_address, byte_length);
      } else {
        writer.CopyLinear(sparse_address, host_address, byte_length);
      }
      offset += byte_length;
    }
    payload_offset += range.byte_length;
  }
  finish_batch();
  return batches;
}

inline void ExecuteSparseTransfer(SparseMappingBackend& backend,
                                  std::span<const SparseCommandBatch> batches) {
  for (const auto& batch : batches) {
    backend.Execute(batch.words, batch.completion_value);
  }
}

inline uint32_t SparsePattern(uint32_t generation, uint32_t index) {
  return (generation << 28) | index;
}

inline bool CheckSparseWords(const char* phase, const uint32_t* words,
                             size_t word_count, uint32_t generation) {
  size_t mismatch_count = 0;
  for (size_t i = 0; i < word_count; ++i) {
    const uint32_t expected =
        SparsePattern(generation, static_cast<uint32_t>(i));
    if (words[i] == expected) {
      continue;
    }
    if (mismatch_count < 8) {
      ADD_FAILURE() << phase << " word=" << i << " expected=" << std::hex
                    << expected << " actual=" << words[i];
    }
    ++mismatch_count;
  }
  std::fprintf(stderr, "%s: checked_words=%zu mismatches=%zu\n", phase,
               word_count, mismatch_count);
  EXPECT_EQ(mismatch_count, 0u) << phase;
  return mismatch_count == 0;
}

inline void ExerciseSparseMappingReplacement(SparseMappingBackend& backend) {
  const auto original_ranges = SparseRanges(1);
  const auto replacement_ranges = SparseRanges(2);
  auto original_mappings = SparseMappings(original_ranges);
  auto replacement_mappings = SparseMappings(replacement_ranges);
  const uint32_t original_count =
      static_cast<uint32_t>(original_mappings.size());
  const uint32_t backing_count = static_cast<uint32_t>(
      std::max(original_mappings.size(), replacement_mappings.size()));
  uint64_t reservation_byte_length = 0;
  uint64_t payload_byte_length = 0;
  for (size_t i = 0; i < original_ranges.size(); ++i) {
    payload_byte_length += original_ranges[i].byte_length;
    for (const auto& range : {original_ranges[i], replacement_ranges[i]}) {
      reservation_byte_length = std::max(reservation_byte_length,
                                         range.byte_offset + range.byte_length);
    }
  }
  reservation_byte_length =
      (reservation_byte_length + kSparseSlabByteLength - 1) &
      ~(kSparseSlabByteLength - 1);
  for (uint32_t i = 0; i < original_count; ++i) {
    original_mappings[i].backing = i;
  }
  for (uint32_t i = 0; i < replacement_mappings.size(); ++i) {
    replacement_mappings[i].backing =
        i < original_count ? original_count - 1 - i : i;
  }
  auto direct_mappings = replacement_mappings;
  for (auto& mapping : direct_mappings) {
    mapping.backing = (mapping.backing + 1) % backing_count;
  }
  backend.Initialize(backing_count, reservation_byte_length,
                     payload_byte_length);
  const size_t word_count = payload_byte_length / sizeof(uint32_t);
  const auto original_upload = SparseTransferCommands(
      backend, original_ranges, original_mappings, SparseTransfer::kUpload, 1);
  const auto original_readback =
      SparseTransferCommands(backend, original_ranges, original_mappings,
                             SparseTransfer::kReadback, 2);
  const auto capture = SparseTransferCommands(
      backend, original_ranges, original_mappings, SparseTransfer::kCapture, 3);
  const auto replacement_upload =
      SparseTransferCommands(backend, replacement_ranges, replacement_mappings,
                             SparseTransfer::kUpload, 4);
  const auto replacement_readback =
      SparseTransferCommands(backend, replacement_ranges, replacement_mappings,
                             SparseTransfer::kReadback, 5);
  const auto physical_readback =
      SparseTransferCommands(backend, replacement_ranges, replacement_mappings,
                             SparseTransfer::kPhysicalReadback, 6);
  const auto final_replacement_reader =
      SparseTransferCommands(backend, replacement_ranges, replacement_mappings,
                             SparseTransfer::kReadback, 7);
  const auto direct_upload = SparseTransferCommands(
      backend, replacement_ranges, direct_mappings, SparseTransfer::kUpload, 8);
  const auto direct_readback =
      SparseTransferCommands(backend, replacement_ranges, direct_mappings,
                             SparseTransfer::kReadback, 9);
  const auto direct_physical_readback =
      SparseTransferCommands(backend, replacement_ranges, direct_mappings,
                             SparseTransfer::kPhysicalReadback, 10);
  for (size_t i = 0; i < word_count; ++i) {
    backend.upload[i] = SparsePattern(1, static_cast<uint32_t>(i));
    backend.readback[i] = 0;
    backend.capture[i] = 0;
  }
  backend.Attach(original_mappings);
  ExecuteSparseTransfer(backend, original_upload);
  ExecuteSparseTransfer(backend, original_readback);
  if (!CheckSparseWords("initial mapping", backend.readback, word_count, 1)) {
    backend.Detach(original_mappings);
    backend.Release();
    return;
  }
  for (size_t i = 0; i < word_count; ++i) {
    backend.upload[i] = SparsePattern(2, static_cast<uint32_t>(i));
  }

  // Final old-generation reader retires here. All CPU data/command preparation
  // precedes this point. Native readiness is the only intervening GPU work.
  ExecuteSparseTransfer(backend, capture);
  backend.Detach(original_mappings);
  backend.Attach(replacement_mappings);
  ExecuteSparseTransfer(backend, replacement_upload);
  ExecuteSparseTransfer(backend, replacement_readback);
  bool replacement_ok =
      CheckSparseWords("replacement mapping", backend.readback, word_count, 2);
  ExecuteSparseTransfer(backend, physical_readback);
  replacement_ok &=
      CheckSparseWords("retained backing", backend.readback, word_count, 2);
  replacement_ok &=
      CheckSparseWords("independent capture", backend.capture, word_count, 1);
  if (!replacement_ok) {
    backend.Detach(replacement_mappings);
    backend.Release();
    return;
  }

  for (size_t i = 0; i < word_count; ++i) {
    backend.upload[i] = SparsePattern(3, static_cast<uint32_t>(i));
  }
  // Keep the occupied destinations live and warm through their final reader.
  // This attach must replace them itself, without an intervening detach.
  ExecuteSparseTransfer(backend, final_replacement_reader);
  backend.Attach(direct_mappings);
  ExecuteSparseTransfer(backend, direct_upload);
  ExecuteSparseTransfer(backend, direct_readback);
  CheckSparseWords("direct replacement", backend.readback, word_count, 3);
  ExecuteSparseTransfer(backend, direct_physical_readback);
  CheckSparseWords("direct backing", backend.readback, word_count, 3);
  CheckSparseWords("preserved capture", backend.capture, word_count, 1);

  // Final teardown knows reservation bounds, not the installed mapping set.
  // It must remove remaining aliases before releasing backing on a live queue.
  backend.Release();
}

}  // namespace amdf::testing

#endif  // AMDF_SRC_GPU_UMD_SPARSE_MAPPING_TEST_H_
