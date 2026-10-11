// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <algorithm>
#include <array>
#include <cstring>
#include <deque>
#include <vector>

#include "libamdf/cts/gpu/kernels/byte_copy_unaligned.h"
#include "libamdf/cts/gpu/kernels/byte_copy_unaligned_kernels.h"
#include "libamdf/cts/gpu/pm4/dispatch_fixture.h"

namespace {

constexpr uint64_t kSlabByteLength = UINT64_C(2) << 20;
constexpr uint64_t kPayloadByteOffset = 512;
constexpr uint64_t kPlacementByteOffset = UINT64_C(18) << 20;
constexpr uint64_t kLargePayloadByteLength = UINT64_C(16) << 20;
constexpr amdf_memory_access_t kReadWrite =
    AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE;

uint32_t TaggedWord(uint32_t owner, uint64_t word) {
  return 0x93b57fd1u ^
         (owner * 0x01010101u + static_cast<uint32_t>(word) * 0x9e3779b9u);
}

class Pm4MappingTest : public Pm4DispatchTest {
 protected:
  void TearDown() override {
    // Remove execution before addresses, then addresses before backing. Native
    // failure never turns into an implicit destructor retry or backing reuse.
    if (queue_ != nullptr) {
      ASSERT_TRUE(queue_->Release(api_));
    }
    if (reservation_ != nullptr) {
      const amdf_status_t status =
          api_->memory_reservation_destroy(reservation_);
      reservation_ = nullptr;
      ASSERT_EQ(status, AMDF_STATUS_OK);
    }
    for (auto& backing : backing_) {
      ASSERT_TRUE(backing.Release(api_));
    }
    Pm4DispatchTest::TearDown();
  }

  void Prepare(amdf_memory_scope_t* scope) {
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
    const amdf_memory_reservation_access_t access = {device_, 0, access_};
    const amdf_memory_reservation_create_info_t create = {
        .type = AMDF_STRUCTURE_TYPE_MEMORY_RESERVATION_CREATE_INFO,
        .structure_size = sizeof(create),
        .byte_length = kReservationByteLength,
        .minimum_alignment = kAlignment,
        .access_count = 1,
        .accesses = &access,
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
    ASSERT_NO_FATAL_FAILURE(CreateQueue(&queue_, 65536));
  }

  void CreateBacking(uint64_t byte_length, GpuMemory** out_memory) {
    const bool system = scope_ == system_scope_;
    const amdf_memory_device_access_t attachment = {
        device_,
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
    create.memory_profile_ordinal = FindMemoryProfileOrdinal(
        scope_, AMDF_MEMORY_PROFILE_ROLE_CREATE, create.required_flags,
        attachment.requirements);
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
    ASSERT_TRUE(original->Release(api_));

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
      ASSERT_TRUE(slabs[i]->Release(api_));
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
    const amdf_status_t status = api_->memory_reservation_destroy(reservation_);
    reservation_ = nullptr;
    ASSERT_EQ(status, AMDF_STATUS_OK);
    for (auto* slab : slabs) {
      ASSERT_TRUE(slab->Release(api_));
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

  // Case-owned VA, explicitly cleared before its ordinary borrowed backing.
  amdf_memory_reservation_t* reservation_ = nullptr;
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

}  // namespace
