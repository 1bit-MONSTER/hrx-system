// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef AMDF_CTS_GPU_UTIL_SPARSE_MAPPING_LAYOUT_H_
#define AMDF_CTS_GPU_UTIL_SPARSE_MAPPING_LAYOUT_H_

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <ios>
#include <span>
#include <vector>

#include "gtest/gtest.h"

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
  // Index of the retained allocation providing this slab.
  uint32_t backing;
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

}  // namespace amdf::testing

#endif  // AMDF_CTS_GPU_UTIL_SPARSE_MAPPING_LAYOUT_H_
