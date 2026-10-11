// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <array>
#include <cstdlib>
#include <cstring>

#include "libamdf/cts/gpu/kernels/byte_copy_unaligned.h"
#include "libamdf/cts/gpu/kernels/byte_copy_unaligned_kernels.h"
#include "libamdf/cts/gpu/pm4/dispatch_fixture.h"

namespace {

constexpr uint32_t kGenerationCount = 8;
constexpr uint64_t kPageLength = 4096;
constexpr uint64_t kBackingLength = 65536;
constexpr uint64_t kTargetOffset = (UINT64_C(5) << 30) + kBackingLength;
constexpr auto kReadWrite = AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE;

uint32_t GenerationWord(uint32_t generation, uint32_t word) {
  return 0x31f893b5u ^ ((generation + 1) * 0x01010101u + word * 0x9e3779b9u);
}

enum class Backing { kSystem, kLocal, kRegistered };

class Pm4QueuedMappingTest : public Pm4DispatchTest,
                             public ::testing::WithParamInterface<Backing> {
 protected:
  Pm4QueuedMappingTest()
      : Pm4DispatchTest(AMDF_QUEUE_PUBLICATION_MODE_KERNEL) {}

  void TearDown() override {
    // Always release the finite predecessor, including an assertion failure.
    if (control_ != nullptr) {
      GpuStoreRelease<uint32_t>(
          reinterpret_cast<uintptr_t>(control_->host.pointer), 1);
    }
    if (queue_ != nullptr) {
      if (last_submission_ != 0) {
        ASSERT_EQ(api_->kernel_queue_wait(queue_, last_submission_,
                                          AMDF_TIMEOUT_INFINITE, 0),
                  AMDF_STATUS_OK);
      }
      const auto status = api_->kernel_queue_destroy(queue_);
      if (status != amdf_make_api_status(AMDF_STATUS_CODE_BUSY)) {
        queue_ = nullptr;
      }
      ASSERT_EQ(status, AMDF_STATUS_OK);
    }
    if (reservation_ != nullptr) {
      const auto status = api_->memory_reservation_destroy(reservation_);
      reservation_ = nullptr;
      ASSERT_EQ(status, AMDF_STATUS_OK);
    }
    for (auto& backing : backing_) {
      ASSERT_TRUE(backing.Release(api_));
    }
    for (void* storage : storage_) {
      std::free(storage);
    }
    Pm4DispatchTest::TearDown();
  }

  void Prepare() {
    amdf_gpu_kernel_queue_create_info_t create_queue = {};
    create_queue.type = AMDF_STRUCTURE_TYPE_GPU_KERNEL_QUEUE_CREATE_INFO;
    create_queue.structure_size = sizeof(create_queue);
    create_queue.queue_family_ordinal = family_.ordinal;
    create_queue.maximum_pending_submission_count = 32;
    ASSERT_EQ(gpu_api_->kernel_queue_create(device_, &create_queue, &queue_),
              AMDF_STATUS_OK);
    amdf_kernel_queue_info_t queue_info = {};
    queue_info.type = AMDF_STRUCTURE_TYPE_KERNEL_QUEUE_INFO;
    queue_info.structure_size = sizeof(queue_info);
    ASSERT_EQ(api_->kernel_queue_query_info(queue_, &queue_info),
              AMDF_STATUS_OK);
    if ((queue_info.capabilities &
         AMDF_KERNEL_QUEUE_CAPABILITY_VIRTUAL_MEMORY) == 0) {
      GTEST_SKIP() << "native queue-ordered mapping is not advertised";
    }
    amdf_memory_reservation_capabilities_t capabilities = {};
    capabilities.type = AMDF_STRUCTURE_TYPE_MEMORY_RESERVATION_CAPABILITIES;
    capabilities.structure_size = sizeof(capabilities);
    ASSERT_EQ(api_->device_query_memory_reservation_capabilities(device_, 0,
                                                                 &capabilities),
              AMDF_STATUS_OK);
    ASSERT_EQ(kPageLength % capabilities.mapping_granularity, 0u);
    access_ = kReadWrite | capabilities.required_access;
    const amdf_memory_reservation_access_t consumer = {device_, 0, access_};
    const amdf_memory_reservation_create_info_t create = {
        .type = AMDF_STRUCTURE_TYPE_MEMORY_RESERVATION_CREATE_INFO,
        .structure_size = sizeof(create),
        .byte_length = UINT64_C(8) << 30,
        .access_count = 1,
        .accesses = &consumer,
    };
    ASSERT_EQ(
        api_->memory_reservation_create(instance_, &create, &reservation_),
        AMDF_STATUS_OK);
    amdf_memory_reservation_access_info_t info = {};
    info.type = AMDF_STRUCTURE_TYPE_MEMORY_RESERVATION_ACCESS_INFO;
    info.structure_size = sizeof(info);
    ASSERT_EQ(
        api_->memory_reservation_query_access_info(reservation_, 0, &info),
        AMDF_STATUS_OK);
    target_address_ = info.address + kTargetOffset;

    auto* scope = GetParam() == Backing::kLocal ? local_scope_ : system_scope_;
    if (scope == nullptr) {
      GTEST_SKIP() << "local backing is not advertised";
    }
    const bool registered = GetParam() == Backing::kRegistered;
    const bool local = GetParam() == Backing::kLocal;
    const amdf_memory_device_access_t attachment = {
        device_,
        {.access = access_,
         .flags = AMDF_MEMORY_FLAG_DEVICE_ADDRESS |
                  AMDF_MEMORY_FLAG_MAPPING_SOURCE |
                  (local ? UINT64_C(0) : AMDF_MEMORY_FLAG_HOST_COHERENT)}};
    const auto flags =
        local ? AMDF_MEMORY_FLAG_DEVICE_LOCAL : AMDF_MEMORY_FLAG_HOST_VISIBLE;
    const auto role = registered ? AMDF_MEMORY_PROFILE_ROLE_REGISTER
                                 : AMDF_MEMORY_PROFILE_ROLE_CREATE;
    const auto ordinal = FindGpuMemoryProfileOrdinal(
        api_, scope, device_, role, flags, attachment.requirements);
    if (ordinal == AMDF_MEMORY_PROFILE_ORDINAL_UNKNOWN) {
      GTEST_SKIP() << "selected backing role is not a mapping source";
    }
    amdf_memory_profile_t profile = {};
    profile.type = AMDF_STRUCTURE_TYPE_MEMORY_PROFILE;
    profile.structure_size = sizeof(profile);
    amdf_memory_access_capabilities_t source_capabilities = {};
    source_capabilities.type = AMDF_STRUCTURE_TYPE_MEMORY_ACCESS_CAPABILITIES;
    source_capabilities.structure_size = sizeof(source_capabilities);
    ASSERT_EQ(
        api_->memory_scope_query_device_profile(scope, ordinal, 1, &attachment,
                                                &profile, &source_capabilities),
        AMDF_STATUS_OK);
    for (uint32_t i = 0; i < backing_.size(); ++i) {
      amdf_memory_create_info_t source = {};
      source.type = AMDF_STRUCTURE_TYPE_MEMORY_CREATE_INFO;
      source.structure_size = sizeof(source);
      source.memory_profile_ordinal = ordinal;
      source.required_flags = flags;
      source.byte_length = kBackingLength;
      source.minimum_alignment = kBackingLength;
      source.access_count = 1;
      source.accesses = &attachment;
      if (registered) {
        ASSERT_LE(profile.registration.registered_host_pointer_alignment,
                  kBackingLength);
        ASSERT_EQ(kBackingLength %
                      profile.registration.native_byte_length_granularity,
                  0u);
        void* storage = std::malloc(2 * kBackingLength);
        ASSERT_NE(storage, nullptr);
        const auto address =
            (reinterpret_cast<uintptr_t>(storage) + kBackingLength - 1) &
            ~(kBackingLength - 1);
        source.registered_host_pointer = reinterpret_cast<void*>(address);
        source.registered_host_cacheability = AMDF_HOST_CACHEABILITY_WRITE_BACK;
        storage_[i] = storage;
        ASSERT_NO_FATAL_FAILURE(backing_[i].Initialize(api_, scope, source));
      } else {
        ASSERT_NO_FATAL_FAILURE(backing_[i].Initialize(api_, scope, source));
      }
    }

    ASSERT_NO_FATAL_FAILURE(CreateMemory(kReadWrite, 4096, &control_));
    ASSERT_NO_FATAL_FAILURE(CreateMemory(
        AMDF_MEMORY_ACCESS_READ, kGenerationCount * kPageLength, &upload_));
    ASSERT_NO_FATAL_FAILURE(CreateMemory(
        kReadWrite, (2 * kGenerationCount + 1) * kPageLength, &capture_));
    ASSERT_NO_FATAL_FAILURE(
        CreateMemory(AMDF_MEMORY_ACCESS_READ, 4096, &arguments_));
    ASSERT_NO_FATAL_FAILURE(
        CreateMemory(AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_EXECUTE,
                     65536, &commands_));
    std::memset(control_->host.pointer, 0, 4096);
    std::memset(capture_->host.pointer, 0, capture_->info.byte_length);
    std::memset(arguments_->host.pointer, 0, arguments_->info.byte_length);
    const auto* kernel =
        kernels::byte_copy_unaligned::kKernels.Find(gpu_endpoint_info_);
    ASSERT_NE(kernel, nullptr);
    program_ = {0,
                kernel->program.resource1,
                kernel->program.resource2,
                kernel->program.resource3,
                kernel->group_segment_byte_length,
                kernel->wavefront_size,
                {kernel->workgroup_size(), 1, 1}};
    ASSERT_NO_FATAL_FAILURE(PrepareProgram(kernel->executable,
                                           kernel->entry_byte_offset, &program_,
                                           "queued_mapping"));
  }

  void EmitCopy(Pm4CommandWriter& writer, uint64_t source,
                uint64_t destination) {
    const uint64_t offset = argument_count_++ * 64;
    const kernels::byte_copy_unaligned::Arguments arguments = {
        source, destination, kPageLength, 1024, 1, program_.workgroup_size[0]};
    std::memcpy(
        static_cast<uint8_t*>(arguments_->host.pointer) + offset, &arguments,
        offsetof(kernels::byte_copy_unaligned::Arguments, workgroup_size_x) +
            sizeof(uint32_t));
    writer.SystemBarrier();
    writer.BindCompute(program_, arguments_->device_address + offset);
    writer.Dispatch(program_, 1024, 1, 1);
    writer.SystemBarrier();
  }

  void Submit(uint64_t offset, uint64_t length) {
    const amdf_gpu_kernel_command_t command = {.memory = commands_->memory,
                                               .byte_offset = offset,
                                               .byte_length = length};
    const amdf_gpu_kernel_queue_submission_info_t submit = {
        .type = AMDF_STRUCTURE_TYPE_GPU_KERNEL_QUEUE_SUBMISSION_INFO,
        .structure_size = sizeof(submit),
        .command_count = 1,
        .commands = &command};
    ASSERT_EQ(gpu_api_->kernel_queue_submit(queue_, &submit, &last_submission_),
              AMDF_STATUS_OK);
  }

  // Existing native queue owns all command and mapping progress points.
  amdf_kernel_queue_t* queue_ = nullptr;
  // Last accepted point, including final detach.
  uint64_t last_submission_ = 0;
  // Stable address owner, destroyed before its borrowed backing.
  amdf_memory_reservation_t* reservation_ = nullptr;
  // Address beyond four GiB in the reserved interval.
  uint64_t target_address_ = 0;
  // Exact native mapping permissions selected from capabilities.
  amdf_memory_access_t access_ = 0;
  // Retained physical slabs permuted while the queue remains alive.
  std::array<GpuMemory, 2> backing_;
  // Caller pages for registered backing, released after checked removal.
  std::array<void*, 2> storage_ = {};
  // Finite host gate and per-generation program completion markers.
  GpuMemory* control_ = nullptr;
  // Immutable source for every generation, prepared before submission.
  GpuMemory* upload_ = nullptr;
  // Independent outputs from both the reservation and original backing alias.
  GpuMemory* capture_ = nullptr;
  // Immutable per-dispatch arguments, never rewritten under active execution.
  GpuMemory* arguments_ = nullptr;
  // Immutable finite command segments, each published once.
  GpuMemory* commands_ = nullptr;
  // Selected executable and resource requirements.
  Pm4ComputeProgram program_ = {};
  // Number of initialized argument slots.
  uint32_t argument_count_ = 0;
};

TEST_P(Pm4QueuedMappingTest, PendingUseThenReplacementThenComputeAndDetach) {
  ASSERT_NO_FATAL_FAILURE(Prepare());
  if (IsSkipped()) {
    return;
  }
  auto* words = static_cast<uint32_t*>(commands_->host.pointer);
  Pm4CommandWriter predecessor(words, *pm4_profile_);
  predecessor.SystemBarrier();
  predecessor.WaitMemory32(control_->device_address, 1,
                           Pm4MemoryComparison::kEqual);
  // This pending predecessor really uses the old mapping: it writes a
  // distinct source page and captures through that page's independent alias.
  EmitCopy(predecessor,
           upload_->device_address + (kGenerationCount - 1) * kPageLength,
           target_address_);
  EmitCopy(predecessor,
           backing_[1].device_address + kBackingLength - kPageLength,
           capture_->device_address + 2 * kGenerationCount * kPageLength);
  predecessor.PadToEightWords();
  ASSERT_LE(predecessor.word_count(), 1024u);
  std::array<uint64_t, kGenerationCount> lengths = {};
  for (uint32_t generation = 0; generation < kGenerationCount; ++generation) {
    auto* source = reinterpret_cast<uint32_t*>(
        static_cast<uint8_t*>(upload_->host.pointer) +
        generation * kPageLength);
    for (uint32_t word = 0; word < kPageLength / 4; ++word) {
      source[word] = GenerationWord(generation, word);
    }
    Pm4CommandWriter writer(words + (generation + 1) * 1024, *pm4_profile_);
    EmitCopy(writer, upload_->device_address + generation * kPageLength,
             target_address_);
    EmitCopy(writer, target_address_,
             capture_->device_address + 2 * generation * kPageLength);
    EmitCopy(writer,
             backing_[generation % 2].device_address +
                 (generation % 4) * kPageLength,
             capture_->device_address + (2 * generation + 1) * kPageLength);
    writer.WriteData32(control_->device_address + 64 + generation * 4,
                       generation + 1);
    writer.PadToEightWords();
    ASSERT_LE(writer.word_count(), 1024u);
    lengths[generation] = writer.word_count() * 4;
  }
  ASSERT_EQ(api_->host_mapping_cache_control(commands_->mapping,
                                             AMDF_HOST_CACHE_OPERATION_FLUSH, 0,
                                             commands_->info.byte_length),
            AMDF_STATUS_OK);
  const amdf_memory_mapping_update_t initial = {
      .target_byte_offset = kTargetOffset,
      .byte_length = kPageLength,
      .memory = backing_[1].memory,
      .access = access_,
      .source_byte_offset = kBackingLength - kPageLength};
  ASSERT_EQ(api_->memory_reservation_update(reservation_, 0, 1, &initial),
            AMDF_STATUS_OK);
  ASSERT_NO_FATAL_FAILURE(Submit(0, predecessor.word_count() * 4));
  for (uint32_t generation = 0; generation < kGenerationCount; ++generation) {
    const amdf_memory_mapping_update_t update = {
        .target_byte_offset = kTargetOffset,
        .byte_length = kPageLength,
        .memory = backing_[generation % 2].memory,
        .access = access_,
        .source_byte_offset = (generation % 4) * kPageLength};
    ASSERT_EQ(api_->kernel_queue_update_memory(queue_, reservation_, 0, 1,
                                               &update, &last_submission_),
              AMDF_STATUS_OK);
    ASSERT_NO_FATAL_FAILURE(
        Submit((generation + 1) * 4096, lengths[generation]));
    if (generation == 0) {
      amdf_kernel_queue_status_t status = {};
      status.type = AMDF_STRUCTURE_TYPE_KERNEL_QUEUE_STATUS;
      status.structure_size = sizeof(status);
      ASSERT_EQ(api_->kernel_queue_refresh_status(queue_, &status),
                AMDF_STATUS_OK);
      EXPECT_EQ(status.retired_submission, 0u);
      EXPECT_EQ(GpuLoadAcquire<uint32_t>(
                    reinterpret_cast<uintptr_t>(control_->host.pointer) + 64),
                0u);
      // Opening only after both mapping and dependent submission returned
      // proves that no host-completed fallback hid inside the queued API.
      GpuStoreRelease<uint32_t>(
          reinterpret_cast<uintptr_t>(control_->host.pointer), 1);
    }
  }
  const amdf_memory_mapping_update_t detach = {
      .target_byte_offset = kTargetOffset, .byte_length = kPageLength};
  ASSERT_EQ(api_->kernel_queue_update_memory(queue_, reservation_, 0, 1,
                                             &detach, &last_submission_),
            AMDF_STATUS_OK);
  ASSERT_EQ(api_->kernel_queue_wait(queue_, last_submission_,
                                    AMDF_TIMEOUT_INFINITE, 0),
            AMDF_STATUS_OK);
  const auto* captured = static_cast<const uint32_t*>(capture_->host.pointer);
  for (uint32_t word = 0; word < kPageLength / 4; ++word) {
    ASSERT_EQ(captured[2 * kGenerationCount * kPageLength / 4 + word],
              GenerationWord(kGenerationCount - 1, word))
        << "pending predecessor word=" << word;
  }
  for (uint32_t generation = 0; generation < kGenerationCount; ++generation) {
    for (uint32_t alias = 0; alias < 2; ++alias) {
      for (uint32_t word = 0; word < kPageLength / 4; ++word) {
        ASSERT_EQ(captured[(2 * generation + alias) * kPageLength / 4 + word],
                  GenerationWord(generation, word))
            << "generation=" << generation << " alias=" << alias
            << " word=" << word;
      }
    }
  }
  // Retire backing before queue/reservation destruction can supply readiness.
  for (auto& backing : backing_) {
    ASSERT_TRUE(backing.Release(api_));
  }
}

INSTANTIATE_TEST_SUITE_P(Backing, Pm4QueuedMappingTest,
                         ::testing::Values(Backing::kSystem, Backing::kLocal,
                                           Backing::kRegistered),
                         [](const ::testing::TestParamInfo<Backing>& info) {
                           switch (info.param) {
                             case Backing::kSystem:
                               return "System";
                             case Backing::kLocal:
                               return "Local";
                             case Backing::kRegistered:
                               return "Registered";
                           }
                           return "Invalid";
                         });

}  // namespace
