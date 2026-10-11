// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <vector>

#include "libamdf/cts/gpu/kernels/byte_copy_unaligned.h"
#include "libamdf/cts/gpu/kernels/byte_copy_unaligned_kernels.h"
#include "libamdf/cts/gpu/pm4/dispatch_fixture.h"
#include "libamdf/cts/gpu/util/sparse_mapping_layout.h"

namespace {

constexpr uint64_t kSlabByteLength = UINT64_C(2) << 20;
constexpr uint64_t kPayloadByteOffset = 512;
constexpr uint64_t kPlacementByteOffset = UINT64_C(18) << 20;
constexpr uint64_t kLargePayloadByteLength = UINT64_C(16) << 20;
constexpr amdf_memory_access_t kReadWrite =
    AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE;

enum class SparseReadRoute { kReservation, kOriginalBacking };

uint32_t TaggedWord(uint32_t owner, uint64_t word) {
  return 0x93b57fd1u ^
         (owner * 0x01010101u + static_cast<uint32_t>(word) * 0x9e3779b9u);
}

class Pm4MappingTest : public Pm4DispatchTest {
 protected:
  void TearDown() override {
    // Remove execution before addresses, then addresses before backing. Native
    // failure never turns into an implicit destructor retry or backing reuse.
    if (cleanup_failed_) {
      return;
    }
    if (queue_ != nullptr) {
      ASSERT_TRUE(queue_->Release(api_));
    }
    for (auto** owner : {&reservation_, &second_reservation_}) {
      if (*owner != nullptr) {
        ASSERT_NO_FATAL_FAILURE(ReleaseReservation(owner));
      }
    }
    for (auto& backing : backing_) {
      ASSERT_NO_FATAL_FAILURE(ReleaseBacking(&backing));
    }
    for (void* storage : caller_storage_) {
      std::free(storage);
    }
    if (second_device_ != nullptr) {
      const amdf_status_t status = api_->device_destroy(second_device_);
      second_device_ = nullptr;
      ASSERT_EQ(status, AMDF_STATUS_OK);
    }
    Pm4DispatchTest::TearDown();
  }

  void ReleaseReservation(amdf_memory_reservation_t** owner) {
    const amdf_status_t status = api_->memory_reservation_destroy(*owner);
    *owner = nullptr;
    cleanup_failed_ = !amdf_status_is_ok(status);
    ASSERT_EQ(status, AMDF_STATUS_OK);
  }

  void ReleaseBacking(GpuMemory* backing) {
    cleanup_failed_ = !backing->Release(api_);
    ASSERT_FALSE(cleanup_failed_);
  }

  void Prepare(amdf_memory_scope_t* scope,
               uint64_t command_byte_length = 65536) {
    ASSERT_NE(scope, nullptr);
    scope_ = scope;
    amdf_memory_reservation_capabilities_t capabilities = {};
    capabilities.type = AMDF_STRUCTURE_TYPE_MEMORY_RESERVATION_CAPABILITIES;
    capabilities.structure_size = sizeof(capabilities);
    const amdf_status_t status =
        api_->device_query_memory_reservation_capabilities(device_, 0,
                                                           &capabilities);
    if (status == amdf_make_api_status(AMDF_STATUS_CODE_UNSUPPORTED)) {
      GTEST_SKIP() << "host-completed GPU mapping updates are unavailable";
    }
    ASSERT_EQ(status, AMDF_STATUS_OK);
    constexpr uint64_t kReservationByteLength = UINT64_C(8) << 40;
    constexpr uint64_t kAlignment = UINT64_C(128) << 20;
    ASSERT_GE(capabilities.maximum_byte_length, kReservationByteLength);
    ASSERT_GE(capabilities.maximum_alignment, kAlignment);
    ASSERT_EQ(kReservationByteLength % capabilities.reservation_granularity,
              0u);
    ASSERT_EQ(kSlabByteLength % capabilities.mapping_granularity, 0u);
    required_access_ = capabilities.required_access;
    access_ = kReadWrite | required_access_;
    const amdf_memory_reservation_access_t accesses[] = {
        {device_, 0, access_}, {second_device_, 0, access_}};
    const amdf_memory_reservation_create_info_t create = {
        .type = AMDF_STRUCTURE_TYPE_MEMORY_RESERVATION_CREATE_INFO,
        .structure_size = sizeof(create),
        .byte_length = kReservationByteLength,
        .minimum_alignment = kAlignment,
        .access_count = second_device_ == nullptr ? 1u : 2u,
        .accesses = accesses,
    };
    ASSERT_EQ(
        api_->memory_reservation_create(instance_, &create, &reservation_),
        AMDF_STATUS_OK);
    reservation_info_.type = AMDF_STRUCTURE_TYPE_MEMORY_RESERVATION_ACCESS_INFO;
    reservation_info_.structure_size = sizeof(reservation_info_);
    ASSERT_EQ(api_->memory_reservation_query_access_info(reservation_, 0,
                                                         &reservation_info_),
              AMDF_STATUS_OK);
    ASSERT_EQ(reservation_info_.address % kAlignment, 0u);
    ASSERT_EQ(reservation_info_.byte_length, kReservationByteLength);
    RecordProperty("mapping_reserved_byte_length",
                   std::to_string(kReservationByteLength));
    RecordProperty("mapping_base", std::to_string(reservation_info_.address));
    RecordProperty("mapping_granularity",
                   std::to_string(reservation_info_.mapping_granularity));

    const auto* kernel =
        kernels::byte_copy_unaligned::kKernels.Find(gpu_endpoint_info_);
    ASSERT_NE(kernel, nullptr) << "missing compiled byte-copy kernel";
    ASSERT_NO_FATAL_FAILURE(CreateMemory(AMDF_MEMORY_ACCESS_READ,
                                         kLargePayloadByteLength, &upload_));
    ASSERT_NO_FATAL_FAILURE(
        CreateMemory(kReadWrite, kLargePayloadByteLength, &readback_));
    ASSERT_NO_FATAL_FAILURE(
        CreateMemory(AMDF_MEMORY_ACCESS_READ, 4096, &arguments_));
    ASSERT_NO_FATAL_FAILURE(CreateMemory(kReadWrite, 4096, &completion_));
    std::memset(completion_->host.pointer, 0, completion_->info.byte_length);
    program_ = {
        0,
        kernel->program.resource1,
        kernel->program.resource2,
        kernel->program.resource3,
        kernel->group_segment_byte_length,
        kernel->wavefront_size,
        {kernel->workgroup_size(), 1, 1},
    };
    ASSERT_NO_FATAL_FAILURE(PrepareProgram(
        kernel->executable, kernel->entry_byte_offset, &program_, "mapping"));
    ASSERT_NO_FATAL_FAILURE(CreateQueue(&queue_, command_byte_length));
  }

  void CreateBacking(uint64_t byte_length, GpuMemory** out_memory,
                     amdf_device_t* consumer = nullptr) {
    if (consumer == nullptr) {
      consumer = device_;
    }
    const bool system = scope_ == system_scope_;
    const amdf_memory_device_access_t attachment = {
        consumer,
        {.access = access_,
         .flags = AMDF_MEMORY_FLAG_DEVICE_ADDRESS |
                  AMDF_MEMORY_FLAG_MAPPING_SOURCE |
                  (system ? AMDF_MEMORY_FLAG_HOST_COHERENT : UINT64_C(0))},
    };
    amdf_memory_create_info_t create = {};
    create.type = AMDF_STRUCTURE_TYPE_MEMORY_CREATE_INFO;
    create.structure_size = sizeof(create);
    create.required_flags =
        system ? AMDF_MEMORY_FLAG_HOST_VISIBLE : AMDF_MEMORY_FLAG_DEVICE_LOCAL;
    create.memory_profile_ordinal = FindGpuMemoryProfileOrdinal(
        api_, scope_, consumer, AMDF_MEMORY_PROFILE_ROLE_CREATE,
        create.required_flags, attachment.requirements);
    ASSERT_NE(create.memory_profile_ordinal,
              AMDF_MEMORY_PROFILE_ORDINAL_UNKNOWN);
    create.byte_length = byte_length;
    create.minimum_alignment = reservation_info_.mapping_granularity;
    create.access_count = 1;
    create.accesses = &attachment;
    auto& memory = backing_.emplace_back();
    ASSERT_NO_FATAL_FAILURE(memory.Initialize(api_, scope_, create));
    *out_memory = &memory;
  }

  void Copy(uint64_t source, uint64_t target, uint64_t byte_length) {
    constexpr uint32_t kGridSize = 1024;
    const kernels::byte_copy_unaligned::Arguments arguments = {
        source, target, byte_length, kGridSize, 1, program_.workgroup_size[0],
    };
    std::memset(arguments_->host.pointer, 0, arguments_->info.byte_length);
    std::memcpy(
        arguments_->host.pointer, &arguments,
        offsetof(kernels::byte_copy_unaligned::Arguments, workgroup_size_x) +
            sizeof(uint32_t));
    // Leave room for the complete fixed compute recipe before writing it. The
    // finite test stream never wraps or overwrites commands already published.
    ASSERT_GE(queue_->words().size() - command_word_count_, 128u);
    Pm4CommandWriter commands(queue_->words().data() + command_word_count_,
                              *pm4_profile_);
    commands.SystemBarrier();
    commands.BindCompute(program_, arguments_->device_address);
    commands.Dispatch(program_, kGridSize, 1, 1);
    commands.SystemBarrier();
    commands.WriteData32(completion_->device_address, ++completed_);
    commands.PadToEightWords();
    command_word_count_ += commands.word_count();
    ASSERT_LE(command_word_count_, queue_->words().size());
    ASSERT_NO_FATAL_FAILURE(
        queue_->Publish(api_, gpu_api_, command_word_count_));
    GpuWaitEqual<uint32_t>(
        reinterpret_cast<uintptr_t>(completion_->host.pointer), completed_);
    // Completion is the program's final system-visible marker. There is no
    // allocation, mapping, native query or queue reconstruction on this edge.
    ASSERT_NO_FATAL_FAILURE(queue_->WaitRetired(api_));
  }

  void Verify(uint64_t address, uint64_t byte_length, uint32_t owner) {
    auto* words = static_cast<uint32_t*>(readback_->host.pointer);
    std::fill_n(words, byte_length / sizeof(uint32_t), 0u);
    ASSERT_NO_FATAL_FAILURE(
        Copy(address, readback_->device_address, byte_length));
    const uint64_t count = byte_length / sizeof(uint32_t);
    uint64_t incorrect = 0;
    uint64_t first = 0;
    for (uint64_t i = 0; i < count; ++i) {
      if (words[i] != TaggedWord(owner, i)) {
        if (incorrect == 0) {
          first = i;
        }
        ++incorrect;
      }
    }
    ASSERT_EQ(incorrect, 0u) << "owner=" << owner << " first_word=" << first
                             << " observed=" << words[first]
                             << " expected=" << TaggedWord(owner, first);
  }

  void WriteAndVerify(uint64_t offset, uint64_t byte_length, uint32_t owner,
                      const GpuMemory& backing) {
    auto* words = static_cast<uint32_t*>(upload_->host.pointer);
    for (uint64_t i = 0; i < byte_length / sizeof(uint32_t); ++i) {
      words[i] = TaggedWord(owner, i);
    }
    const uint64_t address =
        reservation_info_.address + offset + kPayloadByteOffset;
    ASSERT_NO_FATAL_FAILURE(
        Copy(upload_->device_address, address, byte_length));
    ASSERT_NO_FATAL_FAILURE(Verify(address, byte_length, owner));
    // A read through the same stale alias could mask a stale write. The fixed
    // original source projection must observe this generation independently.
    ASSERT_NO_FATAL_FAILURE(Verify(backing.device_address + kPayloadByteOffset,
                                   byte_length, owner));
  }

  void RunSplit(amdf_memory_scope_t* scope) {
    ASSERT_NO_FATAL_FAILURE(Prepare(scope));
    if (IsSkipped()) {
      return;
    }
    GpuMemory* original = nullptr;
    ASSERT_NO_FATAL_FAILURE(CreateBacking(9 * kSlabByteLength, &original));
    amdf_memory_mapping_update_t first = {
        .target_byte_offset = kPlacementByteOffset,
        .byte_length = 9 * kSlabByteLength,
        .memory = original->memory,
        .access = access_,
    };
    ASSERT_EQ(api_->memory_reservation_update(reservation_, 0, 1, &first),
              AMDF_STATUS_OK);
    ASSERT_NO_FATAL_FAILURE(WriteAndVerify(
        kPlacementByteOffset, kLargePayloadByteLength, 100, *original));
    const auto* captured =
        static_cast<const uint32_t*>(readback_->host.pointer);
    const std::vector<uint32_t> capture(
        captured, captured + kLargePayloadByteLength / sizeof(uint32_t));
    first.memory = nullptr;
    first.access = 0;
    ASSERT_EQ(api_->memory_reservation_update(reservation_, 0, 1, &first),
              AMDF_STATUS_OK);
    ASSERT_NO_FATAL_FAILURE(ReleaseBacking(original));

    constexpr std::array<uint64_t, 9> kPayloadLengths = {
        128, 4096, 4096, 4096, 4096, 128, 128, 4096, 4096};
    constexpr std::array<uint32_t, 3> kPhaseEnds = {3, 5, 9};
    std::array<GpuMemory*, 9> slabs = {};
    uint32_t begin = 0;
    for (const uint32_t end : kPhaseEnds) {
      std::array<amdf_memory_mapping_update_t, 4> updates = {};
      for (uint32_t i = begin; i < end; ++i) {
        ASSERT_NO_FATAL_FAILURE(CreateBacking(kSlabByteLength, &slabs[i]));
        updates[i - begin] = {
            .target_byte_offset = kPlacementByteOffset + i * kSlabByteLength,
            .byte_length = kSlabByteLength,
            .memory = slabs[i]->memory,
            .access = access_,
        };
      }
      ASSERT_EQ(api_->memory_reservation_update(reservation_, 0, end - begin,
                                                updates.data()),
                AMDF_STATUS_OK);
      for (uint32_t i = begin; i < end; ++i) {
        ASSERT_NO_FATAL_FAILURE(
            WriteAndVerify(kPlacementByteOffset + i * kSlabByteLength,
                           kPayloadLengths[i], i + 1, *slabs[i]));
      }
      begin = end;
    }
    // Every owner remains mapped while later owners are written. Check them
    // again together, not just each newly mapped range in isolation.
    for (uint32_t i = 0; i < slabs.size(); ++i) {
      ASSERT_NO_FATAL_FAILURE(
          Verify(reservation_info_.address + kPlacementByteOffset +
                     i * kSlabByteLength + kPayloadByteOffset,
                 kPayloadLengths[i], i + 1));
    }
    for (uint64_t i = 0; i < capture.size(); ++i) {
      ASSERT_EQ(capture[i], TaggedWord(100, i));
    }
    constexpr std::array<uint32_t, 9> kReleaseOrder = {0, 2, 1, 6, 5,
                                                       4, 3, 7, 8};
    for (const uint32_t i : kReleaseOrder) {
      const amdf_memory_mapping_update_t detach = {
          .target_byte_offset = kPlacementByteOffset + i * kSlabByteLength,
          .byte_length = kSlabByteLength,
      };
      ASSERT_EQ(api_->memory_reservation_update(reservation_, 0, 1, &detach),
                AMDF_STATUS_OK);
      ASSERT_NO_FATAL_FAILURE(ReleaseBacking(slabs[i]));
    }
    RecordProperty("mapping_verified_split_owners", slabs.size());
  }

  void RunReplacement(amdf_memory_scope_t* scope) {
    ASSERT_NO_FATAL_FAILURE(Prepare(scope));
    if (IsSkipped()) {
      return;
    }
    std::array<GpuMemory*, 3> slabs = {};
    for (auto& slab : slabs) {
      ASSERT_NO_FATAL_FAILURE(CreateBacking(kSlabByteLength, &slab));
    }
    // Descending descriptors also exercise admission and sorting of an ordinary
    // caller vector. Replacement requires no knowledge of the previous owners.
    constexpr uint64_t kHighOffset = (UINT64_C(5) << 30) + kSlabByteLength;
    for (uint32_t generation = 0; generation < slabs.size(); ++generation) {
      std::array<amdf_memory_mapping_update_t, 3> updates = {};
      for (uint32_t i = 0; i < updates.size(); ++i) {
        const uint32_t position = 2 - i;
        updates[i] = {
            .target_byte_offset = kHighOffset + position * kSlabByteLength,
            .byte_length = kSlabByteLength,
            .memory = slabs[(position + generation) % slabs.size()]->memory,
            .access = access_,
        };
      }
      ASSERT_EQ(api_->memory_reservation_update(reservation_, 0, updates.size(),
                                                updates.data()),
                AMDF_STATUS_OK);
      for (uint32_t i = 0; i < slabs.size(); ++i) {
        ASSERT_NO_FATAL_FAILURE(WriteAndVerify(
            kHighOffset + i * kSlabByteLength, 4096, 10 * generation + i + 1,
            *slabs[(i + generation) % slabs.size()]));
      }
      // Reject the entire invalid vector before mutation, including an earlier
      // otherwise valid replacement. The installed generation must survive.
      updates[1].target_byte_offset = updates[0].target_byte_offset;
      updates[0].memory = slabs[generation]->memory;
      ASSERT_EQ(api_->memory_reservation_update(reservation_, 0, updates.size(),
                                                updates.data()),
                amdf_make_api_status(AMDF_STATUS_CODE_INVALID_ARGUMENT));
      for (uint32_t i = 0; i < slabs.size(); ++i) {
        ASSERT_NO_FATAL_FAILURE(Verify(reservation_info_.address + kHighOffset +
                                           i * kSlabByteLength +
                                           kPayloadByteOffset,
                                       4096, 10 * generation + i + 1));
      }
    }
    // Final release clears live mappings and holes while the same queue is
    // still alive. Backing is released before queue teardown can help.
    ASSERT_NO_FATAL_FAILURE(ReleaseReservation(&reservation_));
    for (auto* slab : slabs) {
      ASSERT_NO_FATAL_FAILURE(ReleaseBacking(slab));
    }
    RecordProperty("mapping_verified_retained_generations", 3);
  }

  void RunSourceSlices(amdf_memory_scope_t* scope) {
    ASSERT_NO_FATAL_FAILURE(Prepare(scope));
    if (IsSkipped()) {
      return;
    }
    GpuMemory* backing = nullptr;
    ASSERT_NO_FATAL_FAILURE(CreateBacking(kSlabByteLength, &backing));
    const uint64_t page = reservation_info_.mapping_granularity;
    const amdf_memory_mapping_update_t aliases[] = {
        {.target_byte_offset = kPlacementByteOffset,
         .byte_length = 2 * page,
         .memory = backing->memory,
         .access = access_,
         .source_byte_offset = page},
        {.target_byte_offset = kPlacementByteOffset + kSlabByteLength,
         .byte_length = 2 * page,
         .memory = backing->memory,
         .access = AMDF_MEMORY_ACCESS_READ | required_access_,
         .source_byte_offset = page},
    };
    ASSERT_EQ(api_->memory_reservation_update(reservation_, 0, 2, aliases),
              AMDF_STATUS_OK);
    constexpr uint64_t kPayloadLength = 4096;
    auto* words = static_cast<uint32_t*>(upload_->host.pointer);
    for (uint64_t i = 0; i < kPayloadLength / sizeof(uint32_t); ++i) {
      words[i] = TaggedWord(37, i);
    }
    ASSERT_NO_FATAL_FAILURE(Copy(upload_->device_address,
                                 reservation_info_.address +
                                     aliases[0].target_byte_offset +
                                     kPayloadByteOffset,
                                 kPayloadLength));
    ASSERT_NO_FATAL_FAILURE(Verify(reservation_info_.address +
                                       aliases[1].target_byte_offset +
                                       kPayloadByteOffset,
                                   kPayloadLength, 37));
    ASSERT_NO_FATAL_FAILURE(
        Verify(backing->device_address + page + kPayloadByteOffset,
               kPayloadLength, 37));
    const amdf_memory_mapping_update_t detach = {
        .target_byte_offset = aliases[0].target_byte_offset,
        .byte_length = aliases[0].byte_length,
    };
    ASSERT_EQ(api_->memory_reservation_update(reservation_, 0, 1, &detach),
              AMDF_STATUS_OK);
    // Removing one alias does not remove the source or its other projection.
    ASSERT_NO_FATAL_FAILURE(Verify(reservation_info_.address +
                                       aliases[1].target_byte_offset +
                                       kPayloadByteOffset,
                                   kPayloadLength, 37));
  }

  void WriteSparseGeneration(std::span<const amdf::testing::SparseRange> ranges,
                             const std::array<uint64_t, 2>& addresses,
                             uint32_t generation) {
    auto* input = static_cast<uint32_t*>(upload_->host.pointer);
    uint64_t payload_position = 0;
    for (const auto& range : ranges) {
      for (uint64_t offset = 0; offset < range.byte_length;) {
        const uint64_t length =
            std::min(kLargePayloadByteLength, range.byte_length - offset);
        const uint64_t first_word = (payload_position + offset) / 4;
        for (uint64_t i = 0; i < length / 4; ++i) {
          input[i] = amdf::testing::SparsePattern(
              generation, static_cast<uint32_t>(first_word + i));
        }
        ASSERT_NO_FATAL_FAILURE(Copy(
            upload_->device_address,
            addresses[range.reservation] + range.byte_offset + offset, length));
        offset += length;
      }
      payload_position += range.byte_length;
    }
  }

  void ReadSparseGeneration(
      std::span<const amdf::testing::SparseRange> ranges,
      std::span<const amdf::testing::SparseMapping> mappings,
      std::span<GpuMemory* const> slabs,
      const std::array<uint64_t, 2>& addresses, uint32_t generation,
      SparseReadRoute route, std::span<uint32_t> capture) {
    auto* output = static_cast<uint32_t*>(readback_->host.pointer);
    uint64_t payload_position = 0;
    for (const auto& range : ranges) {
      for (uint64_t offset = 0; offset < range.byte_length;) {
        const uint64_t position = range.byte_offset + offset;
        const uint64_t slab_offset = position % kSlabByteLength;
        const uint64_t length =
            std::min(kSlabByteLength - slab_offset, range.byte_length - offset);
        uint64_t source = addresses[range.reservation] + position;
        if (route == SparseReadRoute::kOriginalBacking) {
          const auto mapping = std::find_if(
              mappings.begin(), mappings.end(), [&](const auto& entry) {
                return entry.reservation == range.reservation &&
                       entry.byte_offset == position - slab_offset;
              });
          ASSERT_NE(mapping, mappings.end());
          source = slabs[mapping->backing]->device_address + slab_offset;
        }
        std::fill_n(output, length / 4, 0u);
        ASSERT_NO_FATAL_FAILURE(
            Copy(source, readback_->device_address, length));
        const uint64_t first_word = (payload_position + offset) / 4;
        uint64_t incorrect = 0;
        uint64_t first_incorrect = 0;
        for (uint64_t i = 0; i < length / 4; ++i) {
          if (output[i] !=
              amdf::testing::SparsePattern(
                  generation, static_cast<uint32_t>(first_word + i))) {
            if (incorrect == 0) {
              first_incorrect = i;
            }
            ++incorrect;
          }
        }
        ASSERT_EQ(incorrect, 0u)
            << "generation=" << generation
            << " reservation=" << range.reservation << " offset=" << position
            << " first_word=" << first_word + first_incorrect
            << " observed=" << output[first_incorrect];
        if (!capture.empty()) {
          ASSERT_LE(first_word + length / 4, capture.size());
          std::copy_n(output, length / 4, capture.data() + first_word);
        }
        offset += length;
      }
      payload_position += range.byte_length;
    }
    std::fprintf(
        stderr, "public compute generation=%u route=%s checked_words=%llu\n",
        generation,
        route == SparseReadRoute::kReservation ? "reservation" : "backing",
        static_cast<unsigned long long>(payload_position / 4));
  }

  void RunRetainedSparse(amdf_memory_scope_t* scope) {
    // The finite stream does not wrap. All backing, command storage and capture
    // storage are prepared before the first mapping generation is installed.
    ASSERT_NO_FATAL_FAILURE(Prepare(scope, UINT64_C(1) << 20));
    if (IsSkipped()) {
      return;
    }
    const auto original_ranges = amdf::testing::SparseRanges(1);
    const auto replacement_ranges = amdf::testing::SparseRanges(2);
    auto original = amdf::testing::SparseMappings(original_ranges);
    auto replacement = amdf::testing::SparseMappings(replacement_ranges);
    const uint32_t original_count = original.size();
    const uint32_t slab_count = std::max(original.size(), replacement.size());
    std::vector<GpuMemory*> slabs(slab_count);
    for (auto& slab : slabs) {
      ASSERT_NO_FATAL_FAILURE(CreateBacking(kSlabByteLength, &slab));
    }
    for (uint32_t i = 0; i < original.size(); ++i) {
      original[i].backing = i;
    }
    for (uint32_t i = 0; i < replacement.size(); ++i) {
      replacement[i].backing = i < original_count ? original_count - 1 - i : i;
    }
    auto direct = replacement;
    for (auto& mapping : direct) {
      mapping.backing = (mapping.backing + 1) % slab_count;
    }
    const amdf_memory_reservation_access_t access = {device_, 0, access_};
    const amdf_memory_reservation_create_info_t create = {
        .type = AMDF_STRUCTURE_TYPE_MEMORY_RESERVATION_CREATE_INFO,
        .structure_size = sizeof(create),
        .byte_length = reservation_info_.byte_length,
        .access_count = 1,
        .accesses = &access,
    };
    ASSERT_EQ(api_->memory_reservation_create(instance_, &create,
                                              &second_reservation_),
              AMDF_STATUS_OK);
    amdf_memory_reservation_access_info_t second = {};
    second.type = AMDF_STRUCTURE_TYPE_MEMORY_RESERVATION_ACCESS_INFO;
    second.structure_size = sizeof(second);
    ASSERT_EQ(api_->memory_reservation_query_access_info(second_reservation_, 0,
                                                         &second),
              AMDF_STATUS_OK);
    const std::array<uint64_t, 2> addresses = {reservation_info_.address,
                                               second.address};
    const std::array<amdf_memory_reservation_t*, 2> reservations = {
        reservation_, second_reservation_};
    using UpdateBatch =
        std::array<std::vector<amdf_memory_mapping_update_t>, 2>;
    const auto make_batch =
        [&](std::span<const amdf::testing::SparseMapping> mappings) {
          UpdateBatch batch;
          for (const auto& mapping : mappings) {
            batch[mapping.reservation].push_back({
                .target_byte_offset = mapping.byte_offset,
                .byte_length = kSlabByteLength,
                .memory = slabs[mapping.backing]->memory,
                .access = access_,
            });
          }
          return batch;
        };
    const auto original_batch = make_batch(original);
    const auto replacement_batch = make_batch(replacement);
    const auto direct_batch = make_batch(direct);
    auto detach_batch = original_batch;
    for (auto& projection : detach_batch) {
      for (auto& entry : projection) {
        entry.memory = nullptr;
        entry.access = 0;
      }
    }
    const auto update = [&](const UpdateBatch& batch) {
      for (uint32_t i = 0; i < batch.size(); ++i) {
        ASSERT_EQ(api_->memory_reservation_update(
                      reservations[i], 0, batch[i].size(), batch[i].data()),
                  AMDF_STATUS_OK);
      }
    };
    uint64_t payload_byte_length = 0;
    for (const auto& range : original_ranges) {
      payload_byte_length += range.byte_length;
    }
    std::vector<uint32_t> capture(payload_byte_length / 4);
    RecordProperty("mapping_sparse_range_count", original_ranges.size());
    RecordProperty("mapping_retained_slab_count", slab_count);
    RecordProperty("mapping_sparse_payload_byte_length",
                   std::to_string(payload_byte_length));

    ASSERT_NO_FATAL_FAILURE(update(original_batch));
    ASSERT_NO_FATAL_FAILURE(
        WriteSparseGeneration(original_ranges, addresses, 1));
    ASSERT_NO_FATAL_FAILURE(
        ReadSparseGeneration(original_ranges, original, slabs, addresses, 1,
                             SparseReadRoute::kReservation, capture));
    ASSERT_NO_FATAL_FAILURE(update(detach_batch));
    ASSERT_NO_FATAL_FAILURE(update(replacement_batch));
    ASSERT_NO_FATAL_FAILURE(
        WriteSparseGeneration(replacement_ranges, addresses, 2));
    ASSERT_NO_FATAL_FAILURE(
        ReadSparseGeneration(replacement_ranges, replacement, slabs, addresses,
                             2, SparseReadRoute::kReservation, {}));
    ASSERT_NO_FATAL_FAILURE(
        ReadSparseGeneration(replacement_ranges, replacement, slabs, addresses,
                             2, SparseReadRoute::kOriginalBacking, {}));
    ASSERT_TRUE(amdf::testing::CheckSparseWords(
        "independent capture", capture.data(), capture.size(), 1));
    ASSERT_NO_FATAL_FAILURE(update(direct_batch));
    ASSERT_NO_FATAL_FAILURE(
        WriteSparseGeneration(replacement_ranges, addresses, 3));
    ASSERT_NO_FATAL_FAILURE(
        ReadSparseGeneration(replacement_ranges, direct, slabs, addresses, 3,
                             SparseReadRoute::kReservation, {}));
    ASSERT_NO_FATAL_FAILURE(
        ReadSparseGeneration(replacement_ranges, direct, slabs, addresses, 3,
                             SparseReadRoute::kOriginalBacking, {}));
    ASSERT_TRUE(amdf::testing::CheckSparseWords(
        "preserved capture", capture.data(), capture.size(), 1));
    for (auto** owner : {&reservation_, &second_reservation_}) {
      ASSERT_NO_FATAL_FAILURE(ReleaseReservation(owner));
    }
    for (auto* slab : slabs) {
      ASSERT_NO_FATAL_FAILURE(ReleaseBacking(slab));
    }
  }

  void RunSharedDomain(amdf_memory_scope_t* scope) {
    // This case deliberately owns a second logical device: its paging owner
    // and allocation handles must work with the first device's existing queue.
    amdf_gpu_device_create_info_t device_create = {};
    device_create.type = AMDF_STRUCTURE_TYPE_GPU_DEVICE_CREATE_INFO;
    device_create.structure_size = sizeof(device_create);
    ASSERT_EQ(
        gpu_api_->device_create(endpoint_, &device_create, &second_device_),
        AMDF_STATUS_OK);
    ASSERT_NO_FATAL_FAILURE(Prepare(scope));
    if (IsSkipped()) {
      return;
    }
    amdf_memory_reservation_access_info_t second = {};
    second.type = AMDF_STRUCTURE_TYPE_MEMORY_RESERVATION_ACCESS_INFO;
    second.structure_size = sizeof(second);
    ASSERT_EQ(
        api_->memory_reservation_query_access_info(reservation_, 1, &second),
        AMDF_STATUS_OK);
    ASSERT_EQ(second.mapping_access_ordinal,
              reservation_info_.mapping_access_ordinal);
    ASSERT_EQ(second.address, reservation_info_.address);

    GpuMemory* primary_backing = nullptr;
    GpuMemory* secondary_backing = nullptr;
    ASSERT_NO_FATAL_FAILURE(CreateBacking(kSlabByteLength, &primary_backing));
    ASSERT_NO_FATAL_FAILURE(
        CreateBacking(kSlabByteLength, &secondary_backing, second_device_));
    const uint64_t offset = (UINT64_C(4) << 30) + kSlabByteLength;
    amdf_memory_mapping_update_t update = {
        .target_byte_offset = offset,
        .byte_length = kSlabByteLength,
        .memory = secondary_backing->memory,
        .access = access_,
    };
    // The first GPU action after publication executes on the other logical
    // device. No allocation or diagnostic submission supplies readiness.
    ASSERT_EQ(api_->memory_reservation_update(reservation_, 1, 1, &update),
              AMDF_STATUS_OK);
    ASSERT_NO_FATAL_FAILURE(
        WriteAndVerify(offset, 4096, 1, *secondary_backing));
    update.memory = primary_backing->memory;
    ASSERT_EQ(api_->memory_reservation_update(reservation_, 0, 1, &update),
              AMDF_STATUS_OK);
    ASSERT_NO_FATAL_FAILURE(WriteAndVerify(offset, 4096, 2, *primary_backing));
    ASSERT_NO_FATAL_FAILURE(Verify(
        secondary_backing->device_address + kPayloadByteOffset, 4096, 1));
    ASSERT_NO_FATAL_FAILURE(ReleaseReservation(&reservation_));
    ASSERT_NO_FATAL_FAILURE(ReleaseBacking(secondary_backing));
    ASSERT_NO_FATAL_FAILURE(ReleaseBacking(primary_backing));
  }

  void RunRegisteredSource() {
    ASSERT_NO_FATAL_FAILURE(Prepare(system_scope_));
    if (IsSkipped()) {
      return;
    }
    const amdf_memory_device_access_t attachment = {
        device_,
        {.access = access_,
         .flags = AMDF_MEMORY_FLAG_DEVICE_ADDRESS |
                  AMDF_MEMORY_FLAG_MAPPING_SOURCE |
                  AMDF_MEMORY_FLAG_HOST_COHERENT},
    };
    amdf_memory_scope_info_t scope_info = {};
    scope_info.type = AMDF_STRUCTURE_TYPE_MEMORY_SCOPE_INFO;
    scope_info.structure_size = sizeof(scope_info);
    ASSERT_EQ(api_->memory_scope_query_info(scope_, &scope_info),
              AMDF_STATUS_OK);
    amdf_memory_profile_t profile = {};
    profile.type = AMDF_STRUCTURE_TYPE_MEMORY_PROFILE;
    profile.structure_size = sizeof(profile);
    amdf_memory_access_capabilities_t capabilities = {};
    capabilities.type = AMDF_STRUCTURE_TYPE_MEMORY_ACCESS_CAPABILITIES;
    capabilities.structure_size = sizeof(capabilities);
    uint32_t ordinal = AMDF_MEMORY_PROFILE_ORDINAL_UNKNOWN;
    for (uint32_t i = 0; i < scope_info.memory_profile_count; ++i) {
      const amdf_status_t status = api_->memory_scope_query_device_profile(
          scope_, i, 1, &attachment, &profile, &capabilities);
      if (status == amdf_make_api_status(AMDF_STATUS_CODE_UNSUPPORTED)) {
        continue;
      }
      ASSERT_EQ(status, AMDF_STATUS_OK);
      constexpr auto kRoles =
          AMDF_MEMORY_PROFILE_ROLE_REGISTER | AMDF_MEMORY_PROFILE_ROLE_HOST_MAP;
      if ((profile.roles & kRoles) == kRoles &&
          (profile.supported_flags & AMDF_MEMORY_FLAG_HOST_VISIBLE) != 0) {
        ordinal = i;
        break;
      }
    }
    if (ordinal == AMDF_MEMORY_PROFILE_ORDINAL_UNKNOWN) {
      GTEST_SKIP() << "registered pages are not an advertised mapping source";
    }
    const uint64_t granularity =
        std::max(reservation_info_.mapping_granularity,
                 profile.registration.native_byte_length_granularity);
    const uint64_t alignment =
        std::max({granularity, profile.registration.minimum_alignment,
                  profile.registration.registered_host_pointer_alignment});
    ASSERT_GE(granularity, 4096u);
    ASSERT_LE(granularity, kLargePayloadByteLength);
    const uint64_t byte_length = 4 * granularity;
    ASSERT_LE(byte_length, profile.registration.maximum_byte_length);
    std::array<GpuMemory*, 2> sources = {};
    for (auto& source : sources) {
      void* storage = std::malloc(byte_length + alignment - 1);
      ASSERT_NE(storage, nullptr);
      const uintptr_t pointer =
          (reinterpret_cast<uintptr_t>(storage) + alignment - 1) &
          ~(alignment - 1);
      std::memset(reinterpret_cast<void*>(pointer), 0, byte_length);
      amdf_memory_create_info_t create = {};
      create.type = AMDF_STRUCTURE_TYPE_MEMORY_CREATE_INFO;
      create.structure_size = sizeof(create);
      create.memory_profile_ordinal = ordinal;
      create.required_flags = AMDF_MEMORY_FLAG_HOST_VISIBLE;
      create.byte_length = byte_length;
      create.minimum_alignment = alignment;
      create.access_count = 1;
      create.accesses = &attachment;
      create.registered_host_pointer = reinterpret_cast<void*>(pointer);
      create.registered_host_cacheability = AMDF_HOST_CACHEABILITY_WRITE_BACK;
      source = &backing_.emplace_back();
      source->Initialize(api_, scope_, create);
      // A failed construction rollback can retain native pins. Free caller
      // pages only through a published handle's successful final release.
      if (source->memory != nullptr) {
        caller_storage_.push_back(storage);
      }
      ASSERT_FALSE(HasFatalFailure());
    }
    const uint64_t offset = (UINT64_C(4) << 30) + kSlabByteLength;
    const uint64_t payload_length = granularity - kPayloadByteOffset;
    amdf_memory_mapping_update_t update = {
        .target_byte_offset = offset,
        .byte_length = granularity,
        .access = access_,
        .source_byte_offset = granularity,
    };
    for (uint32_t generation = 1; generation <= sources.size(); ++generation) {
      update.memory = sources[generation - 1]->memory;
      auto* words = static_cast<uint32_t*>(upload_->host.pointer);
      for (uint64_t i = 0; i < payload_length / 4; ++i) {
        words[i] = TaggedWord(generation, i);
      }
      ASSERT_EQ(api_->memory_reservation_update(reservation_, 0, 1, &update),
                AMDF_STATUS_OK);
      ASSERT_NO_FATAL_FAILURE(
          Copy(upload_->device_address,
               reservation_info_.address + offset + kPayloadByteOffset,
               payload_length));
      ASSERT_NO_FATAL_FAILURE(
          Verify(reservation_info_.address + offset + kPayloadByteOffset,
                 payload_length, generation));
      for (uint32_t previous = 1; previous <= generation; ++previous) {
        const auto* source = sources[previous - 1];
        ASSERT_NO_FATAL_FAILURE(
            Verify(source->device_address + granularity + kPayloadByteOffset,
                   payload_length, previous));
        const auto* host = reinterpret_cast<const uint32_t*>(
            static_cast<const uint8_t*>(
                source->creation.registered_host_pointer) +
            granularity + kPayloadByteOffset);
        for (uint64_t i = 0; i < payload_length / 4; ++i) {
          ASSERT_EQ(host[i], TaggedWord(previous, i)) << "word=" << i;
        }
      }
    }
    ASSERT_NO_FATAL_FAILURE(ReleaseReservation(&reservation_));
    for (auto* source : sources) {
      ASSERT_NO_FATAL_FAILURE(ReleaseBacking(source));
    }
  }

  // Case-owned VA, explicitly cleared before its ordinary borrowed backing.
  amdf_memory_reservation_t* reservation_ = nullptr;
  // Second independent address interval in the retained-slab scenario.
  amdf_memory_reservation_t* second_reservation_ = nullptr;
  // Immutable consumer projection queried before physical backing exists.
  amdf_memory_reservation_access_info_t reservation_info_ = {};
  // Borrowed storage placement chosen by the scenario.
  amdf_memory_scope_t* scope_ = nullptr;
  // Exact permissions supported by this native mapping domain.
  amdf_memory_access_t access_ = 0;
  // Permissions that native hardware cannot deny on an attached alias.
  amdf_memory_access_t required_access_ = 0;
  // Stable physical owners; early release consumes handles without erasing
  // slots.
  std::deque<GpuMemory> backing_;
  // Caller-owned pages retained through every registered source's teardown.
  std::vector<void*> caller_storage_;
  // Case-owned logical device used to exercise shared native address domains.
  amdf_device_t* second_device_ = nullptr;
  // Failed consuming teardown preserves the remaining native dependencies.
  bool cleanup_failed_ = false;
  // Persistent stream, owned by the command fixture and never recreated.
  GpuCommandQueue* queue_ = nullptr;
  // Immutable compiled program selected by endpoint capabilities.
  Pm4ComputeProgram program_ = {};
  // Fixed CPU-authored source outside the remapped reservation.
  GpuMemory* upload_ = nullptr;
  // Fixed output observing the first GPU access after each change.
  GpuMemory* readback_ = nullptr;
  // Reusable argument storage modified only after execution completion.
  GpuMemory* arguments_ = nullptr;
  // Program-ordered completion marker, distinct from ring consumption.
  GpuMemory* completion_ = nullptr;
  // Cumulative finite command extent; the stream does not wrap.
  size_t command_word_count_ = 0;
  // Monotonic completion value for the finite dispatch sequence.
  uint32_t completed_ = 0;
};

TEST_F(Pm4MappingTest, SystemBackingSplitReuseIsImmediatelyVisibleToCompute) {
  RunSplit(system_scope_);
}

TEST_F(Pm4MappingTest, LocalBackingSplitReuseIsImmediatelyVisibleToCompute) {
  if (local_scope_ == nullptr) {
    GTEST_SKIP() << "endpoint has no local storage scope";
  }
  RunSplit(local_scope_);
}

TEST_F(Pm4MappingTest,
       SystemBackingReplacementPreservesOtherAliasesAndRejectsOverlap) {
  RunReplacement(system_scope_);
}

TEST_F(Pm4MappingTest,
       LocalBackingReplacementPreservesOtherAliasesAndRejectsOverlap) {
  if (local_scope_ == nullptr) {
    GTEST_SKIP() << "endpoint has no local storage scope";
  }
  RunReplacement(local_scope_);
}

TEST_F(Pm4MappingTest, SystemBackingSlicesAndAliasesSharePhysicalStorage) {
  RunSourceSlices(system_scope_);
}

TEST_F(Pm4MappingTest, LocalBackingSlicesAndAliasesSharePhysicalStorage) {
  if (local_scope_ == nullptr) {
    GTEST_SKIP() << "endpoint has no local storage scope";
  }
  RunSourceSlices(local_scope_);
}

TEST_F(Pm4MappingTest, SystemBackingPermutesRetainedSparseStateAndCacheRanges) {
  RunRetainedSparse(system_scope_);
}

TEST_F(Pm4MappingTest, LocalBackingPermutesRetainedSparseStateAndCacheRanges) {
  if (local_scope_ == nullptr) {
    GTEST_SKIP() << "endpoint has no local storage scope";
  }
  RunRetainedSparse(local_scope_);
}

TEST_F(Pm4MappingTest, SystemBackingReplacementThroughSharedLogicalDevices) {
  RunSharedDomain(system_scope_);
}

TEST_F(Pm4MappingTest, LocalBackingReplacementThroughSharedLogicalDevices) {
  if (local_scope_ == nullptr) {
    GTEST_SKIP() << "endpoint has no local storage scope";
  }
  RunSharedDomain(local_scope_);
}

TEST_F(Pm4MappingTest, RegisteredBackingReplacementPreservesCallerPages) {
  RunRegisteredSource();
}

}  // namespace
