// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#include "libamdf/cts/gpu/aql/dispatch_fixture.h"
#include "libamdf/cts/gpu/kernels/lds_exchange.h"
#include "libamdf/cts/gpu/kernels/lds_exchange_kernels.h"
#include "libamdf/cts/gpu/kernels/private_roundtrip.h"
#include "libamdf/cts/gpu/kernels/private_roundtrip_gfx942.h"

namespace {

namespace private_kernel = kernels::gfx942_private_roundtrip;

static_assert(private_kernel::kArgumentByteOffsets ==
              kernels::private_roundtrip::kArgumentByteOffsets);
static_assert(private_kernel::kArgumentByteLengths ==
              kernels::private_roundtrip::kArgumentByteLengths);
static_assert(private_kernel::kArgumentValueKinds ==
              kernels::private_roundtrip::kArgumentValueKinds);

TEST_F(AqlDispatchTest, SwitchesBetweenPrivateAndLdsKernels) {
  constexpr uint32_t kGridSize = 512;
  constexpr uint32_t kPrivateOutputWordCount = kGridSize * 9;
  constexpr uint32_t kLdsOutputWordCount = kGridSize * 2;
  constexpr uint32_t kGuardWordCount = 16;
  constexpr uint32_t kWordCount = kPrivateOutputWordCount + 2 * kGuardWordCount;
  constexpr uint32_t kPageByteLength = 4096;
  constexpr uint32_t kArgumentSlotByteLength = 32;
  constexpr uint32_t kCompletionGuardWordCount =
      (kPageByteLength - sizeof(aql::Signal)) / sizeof(uint32_t);
  constexpr uint32_t kPrefixGuard = 0x619b30d5u;
  constexpr uint32_t kSuffixGuard = 0xe270c84bu;
  constexpr uint32_t kCompletionGuard = 0x68d329b7u;
  // Four serial epochs cover both private->LDS->private and LDS->private->LDS.
  constexpr std::array<uint32_t, 4> kSeeds = {0x13579bdfu, 0x2468ace1u,
                                              0xa5c31f27u, 0x8db462f3u};
  constexpr std::array<uint32_t, 2> kRotations = {1, 7};
  constexpr aql::FenceScopes kScopes = {aql::FenceScope::kSystem,
                                        aql::FenceScope::kSystem};
  static_assert(private_kernel::kWorkgroupSize == 64);
  static_assert(private_kernel::kPrivateSegmentByteLength > 0);
  static_assert(private_kernel::kGroupSegmentByteLength == 0);

  static_assert(sizeof(kernels::private_roundtrip::Arguments) ==
                private_kernel::kKernargByteLength);
  static_assert(alignof(kernels::private_roundtrip::Arguments) %
                    private_kernel::kKernargAlignment ==
                0);

  static_assert(sizeof(kernels::lds_exchange::Arguments) <=
                kArgumentSlotByteLength);

  amdf_gpu_endpoint_info_t endpoint_info = {};
  endpoint_info.type = AMDF_STRUCTURE_TYPE_GPU_ENDPOINT_INFO;
  endpoint_info.structure_size = sizeof(endpoint_info);
  ASSERT_EQ(gpu_api_->endpoint_query_info(endpoint_, &endpoint_info),
            AMDF_STATUS_OK);
  const auto* selected = kernels::lds_exchange::kKernels.Find(endpoint_info);
  ASSERT_NE(selected, nullptr) << "missing compiled LDS kernel for endpoint";
  const auto& lds_kernel = *selected;
  RecordProperty("lds_kernel_target", lds_kernel.target);

  ASSERT_LE(lds_kernel.group_segment_byte_length,
            endpoint_info.compute.local_data_share_byte_length);

  GpuMemory* output = nullptr;
  GpuMemory* arguments = nullptr;
  GpuMemory* completion = nullptr;
  ASSERT_NO_FATAL_FAILURE(
      CreateMemory(AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE,
                   uint64_t{kWordCount} * sizeof(uint32_t), &output));
  ASSERT_NO_FATAL_FAILURE(
      CreateMemory(AMDF_MEMORY_ACCESS_READ, kPageByteLength, &arguments));
  ASSERT_NO_FATAL_FAILURE(
      CreateMemory(AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE,
                   kPageByteLength, &completion));
  ASSERT_EQ(arguments->device_address % private_kernel::kKernargAlignment, 0u);
  ASSERT_EQ(arguments->device_address % lds_kernel.arguments.alignment, 0u);
  ASSERT_EQ(completion->device_address % alignof(aql::Signal), 0u);
  std::memset(arguments->host.pointer, 0, arguments->info.byte_length);
  std::memset(completion->host.pointer, 0, completion->info.byte_length);
  auto& signal = *static_cast<aql::Signal*>(completion->host.pointer);
  signal.kind = 1;
  auto* completion_guard_address =
      static_cast<uint8_t*>(completion->host.pointer) + sizeof(aql::Signal);
  std::array<uint32_t, kCompletionGuardWordCount> completion_guards;
  completion_guards.fill(kCompletionGuard);
  std::memcpy(completion_guard_address, completion_guards.data(),
              sizeof(completion_guards));

  GpuUserQueue* queue = nullptr;
  ASSERT_NO_FATAL_FAILURE(CreateFixedScratchQueue(
      private_kernel::kPrivateSegmentByteLength, &queue));
  uint64_t index = 0;
  uint64_t private_descriptor_address = 0;
  uint64_t lds_descriptor_address = 0;
  ASSERT_NO_FATAL_FAILURE(PublishKernel(*queue, private_kernel::kExecutable,
                                        "aql_transition_private", &index,
                                        &private_descriptor_address));
  ASSERT_NO_FATAL_FAILURE(PublishKernel(*queue, lds_kernel.executable,
                                        "aql_transition_lds", &index,
                                        &lds_descriptor_address));
  const uint64_t first_work_packet_index = index;
  const uint64_t capacity = queue->host.ring_byte_length / sizeof(aql::Packet);
  ASSERT_GE(capacity, index + kSeeds.size());

  RecordProperty("aql_transition_private_entry_byte_offset",
                 private_kernel::kEntryByteOffset);
  RecordProperty("aql_transition_private_private_segment_byte_length",
                 private_kernel::kPrivateSegmentByteLength);
  RecordProperty("aql_transition_private_fixed_group_byte_length",
                 private_kernel::kGroupSegmentByteLength);
  RecordProperty("aql_transition_private_workgroup_size",
                 private_kernel::kWorkgroupSize);
  RecordProperty("aql_transition_private_kernarg_byte_length",
                 private_kernel::kKernargByteLength);
  RecordProperty("aql_transition_lds_entry_byte_offset",
                 lds_kernel.entry_byte_offset);
  RecordProperty("aql_transition_lds_private_segment_byte_length",
                 lds_kernel.private_segment_byte_length);
  RecordProperty("aql_transition_lds_fixed_group_byte_length",
                 lds_kernel.group_segment_byte_length);
  RecordProperty("aql_transition_lds_workgroup_size",
                 lds_kernel.workgroup_size());
  RecordProperty("aql_transition_lds_kernarg_byte_length",
                 lds_kernel.arguments.byte_length);
  RecordProperty("aql_resource_transition_sequence", "private,lds,private,lds");
  RecordProperty("aql_resource_transition_lds_capacity_per_compute_unit",
                 endpoint_info.compute.local_data_share_byte_length);
  RecordProperty("aql_resource_transition_checked_output_words", kWordCount);
  RecordProperty("aql_resource_transition_kernarg_slot_byte_length",
                 kArgumentSlotByteLength);
  RecordProperty("aql_resource_transition_cold_publication_count",
                 std::to_string(first_work_packet_index));
  RecordProperty("aql_resource_transition_first_work_packet_index",
                 std::to_string(first_work_packet_index));
  RecordProperty("aql_resource_transition_ring_capacity_packets",
                 std::to_string(capacity));

  std::array<uint32_t, kWordCount> expected_output;
  std::array<uint32_t, kWordCount> observed_output;
  std::array<uint8_t, kPageByteLength> expected_arguments;
  std::array<uint8_t, kPageByteLength> observed_arguments;
  aql::Signal observed_signal = {};
  for (uint32_t epoch = 0; epoch < kSeeds.size(); ++epoch) {
    SCOPED_TRACE(epoch);
    const bool uses_private = epoch % 2 == 0;
    const uint32_t image_epoch = epoch / 2;
    const uint32_t active_word_count =
        uses_private ? kPrivateOutputWordCount : kLdsOutputWordCount;
    const uint32_t private_byte_length =
        uses_private ? private_kernel::kPrivateSegmentByteLength
                     : lds_kernel.private_segment_byte_length;
    const uint32_t group_byte_length =
        uses_private ? private_kernel::kGroupSegmentByteLength
                     : lds_kernel.group_segment_byte_length;
    const uint16_t workgroup_size = uses_private
                                        ? private_kernel::kWorkgroupSize
                                        : lds_kernel.workgroup_size();
    expected_output.fill(kSuffixGuard);
    for (uint32_t word = 0; word < kGuardWordCount; ++word) {
      expected_output[word] = kPrefixGuard;
    }
    for (uint32_t word = 0; word < kPrivateOutputWordCount; ++word) {
      expected_output[kGuardWordCount + word] =
          0x46c2a9d3u ^ static_cast<uint32_t>(uint64_t{word} * 0x9e3779b9u);
    }
    expected_arguments.fill(0);
    if (uses_private) {
      for (uint32_t workitem = 0; workitem < kGridSize; ++workitem) {
        const auto record = kernels::private_roundtrip::ExpectedRecord(
            workitem, kSeeds[epoch], kRotations[image_epoch]);
        std::memcpy(
            expected_output.data() + kGuardWordCount + workitem * record.size(),
            record.data(), sizeof(record));
      }
      const kernels::private_roundtrip::Arguments payload = {
          output->device_address + kGuardWordCount * sizeof(uint32_t),
          kSeeds[epoch],
          kRotations[image_epoch],
      };
      std::memcpy(expected_arguments.data(), &payload,
                  private_kernel::kKernargByteLength);
    } else {
      for (uint32_t workitem = 0; workitem < kGridSize; ++workitem) {
        const auto record =
            kernels::lds_exchange::ExpectedRecord(workitem, kSeeds[epoch]);
        std::memcpy(
            expected_output.data() + kGuardWordCount + workitem * record.size(),
            record.data(), sizeof(record));
      }
      const kernels::lds_exchange::Arguments payload = {
          output->device_address + kGuardWordCount * sizeof(uint32_t),
          kSeeds[epoch],
      };
      std::memcpy(expected_arguments.data(), &payload,
                  kernels::lds_exchange::kArgumentByteLength);
    }
    observed_output = expected_output;
    for (uint32_t word = 0; word < active_word_count; ++word) {
      observed_output[kGuardWordCount + word] =
          ~expected_output[kGuardWordCount + word];
    }
    std::memcpy(output->host.pointer, observed_output.data(),
                sizeof(observed_output));
    // The full 32-byte slot backs each image's scalar fetch with initialized
    // bytes. Only semantic fields come from the typed object; the tail is zero.
    std::memcpy(arguments->host.pointer, expected_arguments.data(),
                kArgumentSlotByteLength);
    ASSERT_EQ(
        GpuLoadAcquire<int64_t>(reinterpret_cast<uintptr_t>(&signal.value)), 0);
    signal.value = 1;
    const auto packet = aql::Dispatch(
        aql::HeaderBarrier::kDisabled,
        {1, {workgroup_size, 1, 1}, {kGridSize, 1, 1}}, private_byte_length,
        group_byte_length,
        uses_private ? private_descriptor_address : lds_descriptor_address,
        arguments->device_address, completion->device_address, kScopes);
    GpuStoreRelease(queue->host.write_index_address, index + 1);
    Publish(*queue, index++, packet);

    // The complete initialized output extent, arguments and signal storage
    // are captured before diagnostics or consumption can add synchronization.
    GpuWaitEqual<int64_t>(reinterpret_cast<uintptr_t>(&signal.value), 0);
    std::memcpy(observed_output.data(), output->host.pointer,
                sizeof(observed_output));
    std::memcpy(observed_arguments.data(), arguments->host.pointer,
                sizeof(observed_arguments));
    std::memcpy(&observed_signal, completion->host.pointer,
                sizeof(observed_signal));
    std::memcpy(completion_guards.data(), completion_guard_address,
                sizeof(completion_guards));
    for (uint32_t word = 0; word < kWordCount; ++word) {
      EXPECT_EQ(observed_output[word], expected_output[word])
          << "output word=" << word;
    }
    for (uint32_t byte = 0; byte < kPageByteLength; ++byte) {
      EXPECT_EQ(observed_arguments[byte], expected_arguments[byte])
          << "kernarg byte=" << byte;
    }
    EXPECT_EQ(observed_signal.kind, 1);
    EXPECT_EQ(observed_signal.value, 0);
    for (uint32_t word = 0; word < completion_guards.size(); ++word) {
      EXPECT_EQ(completion_guards[word], kCompletionGuard)
          << "completion guard word=" << word;
    }
    // Preserve retirement on every nonfatal mismatch. Reusing the argument
    // slot, output or signal requires success; the queue still borrows scratch.
    EXPECT_NO_FATAL_FAILURE(queue->WaitConsumed(api_, index));
    if (HasFailure()) {
      return;
    }
    const std::string prefix =
        "aql_resource_transition_epoch_" + std::to_string(epoch + 1);
    RecordProperty(prefix + "_image_kind", uses_private ? "private" : "lds");
    RecordProperty(prefix + "_seed", std::to_string(kSeeds[epoch]));
    RecordProperty(prefix + "_private_segment_byte_length",
                   private_byte_length);
    RecordProperty(prefix + "_group_segment_byte_length", group_byte_length);
    RecordProperty(prefix + "_workgroup_size", workgroup_size);
    RecordProperty(prefix + "_grid_size", kGridSize);
    RecordProperty(prefix + "_active_word_count", active_word_count);
    RecordProperty(prefix + "_inactive_word_count",
                   kPrivateOutputWordCount - active_word_count);
    RecordProperty(prefix + "_packet_index", std::to_string(index - 1));
    if (uses_private) {
      RecordProperty(prefix + "_rotation", kRotations[image_epoch]);
    }
  }
  RecordProperty("aql_resource_transition_completed_epochs", kSeeds.size());
  RecordProperty("aql_resource_transition_private_completed_epochs",
                 kRotations.size());
  RecordProperty("aql_resource_transition_lds_completed_epochs",
                 kSeeds.size() / 2);
  RecordProperty("aql_resource_transition_work_packet_count",
                 std::to_string(index - first_work_packet_index));
  RecordProperty("aql_resource_transition_final_packet_index",
                 std::to_string(index));
}

}  // namespace
