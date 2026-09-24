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

#include "libamdf/cts/gpu/kernels/transform.h"
#include "libamdf/cts/gpu/kernels/transform_gfx1151.h"
#include "libamdf/cts/gpu/pm4/dispatch_fixture.h"

namespace {

namespace kernel = kernels::gfx1151_transform;

TEST_F(Pm4DispatchTest, SelectsImmutableIndirectWorkgroupCounts) {
  constexpr uint32_t kWordCount = 2048;
  constexpr uint32_t kPayloadOffset = 16;
  constexpr uint32_t kCount = 1536;
  constexpr uint32_t kPageByteLength = 4096;
  constexpr uint32_t kPageWordCount = kPageByteLength / sizeof(uint32_t);
  constexpr uint32_t kCompletionByteOffset = 256;
  constexpr uint32_t kCompletionWordIndex =
      kCompletionByteOffset / sizeof(uint32_t);
  constexpr uint32_t kCommandWordsPerEpoch = 64;
  constexpr std::array<uint32_t, 2> kTupleByteOffsets = {256, 320};
  constexpr std::array<uint32_t, 2> kWorkgroupCounts = {16, 9};
  constexpr std::array<uint32_t, 2> kActiveCounts = {1024, 576};
  constexpr std::array<uint32_t, 2> kAddends = {7, 0x80000023u};
  static_assert(kPayloadOffset + kCount <= kWordCount);
  static_assert(kernel::kWorkgroupSize == 64);
  static_assert(alignof(kernels::transform::Arguments) ==
                kernel::kKernargAlignment);
  static_assert(offsetof(kernels::transform::Arguments, addend) +
                    sizeof(uint32_t) ==
                kernel::kKernargByteLength);

  GpuMemory* input = nullptr;
  GpuMemory* output = nullptr;
  GpuMemory* arguments = nullptr;
  GpuMemory* tuples = nullptr;
  GpuMemory* completion = nullptr;
  GpuMemory* code = nullptr;
  ASSERT_NO_FATAL_FAILURE(CreateMemory(AMDF_MEMORY_ACCESS_READ,
                                       kWordCount * sizeof(uint32_t), &input));
  ASSERT_NO_FATAL_FAILURE(
      CreateMemory(AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE,
                   kWordCount * sizeof(uint32_t), &output));
  ASSERT_NO_FATAL_FAILURE(
      CreateMemory(AMDF_MEMORY_ACCESS_READ, kPageByteLength, &arguments));
  ASSERT_NO_FATAL_FAILURE(
      CreateMemory(AMDF_MEMORY_ACCESS_READ, kPageByteLength, &tuples));
  ASSERT_NO_FATAL_FAILURE(
      CreateMemory(AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE,
                   kPageByteLength, &completion));
  ASSERT_EQ(arguments->device_address % kernel::kKernargAlignment, 0u);
  ASSERT_EQ(tuples->device_address % sizeof(uint32_t), 0u);
  ASSERT_EQ(completion->device_address % sizeof(uint32_t), 0u);
  Pm4ComputeProgram program = {0,
                               kernel::kComputePgmRsrc1,
                               kernel::kComputePgmRsrc2,
                               kernel::kComputePgmRsrc3,
                               kernel::kGroupSegmentByteLength,
                               {kernel::kWorkgroupSize, 1, 1}};
  ASSERT_NO_FATAL_FAILURE(PrepareProgram(kernel::kExecutable,
                                         kernel::kEntryByteOffset, &program,
                                         "pm4_indirect", &code));

  std::array<uint32_t, kWordCount> expected_input, observed_input;
  std::array<uint32_t, kWordCount> expected_output, observed_output;
  std::array<uint8_t, kPageByteLength> expected_arguments, observed_arguments;
  std::array<uint32_t, kPageWordCount> expected_tuples, observed_tuples;
  std::array<uint32_t, kPageWordCount> expected_control, observed_control;
  std::array<uint8_t, kPageByteLength> expected_code = {};
  std::array<uint8_t, kPageByteLength> observed_code;
  std::memcpy(expected_code.data(), kernel::kExecutable.words,
              kernel::kExecutable.byte_length);
  for (uint32_t i = 0; i < kPageWordCount; ++i) {
    expected_tuples[i] = 0x9137ace5u ^ i;
    expected_control[i] = 0x68d329b7u ^ i;
  }
  for (uint32_t epoch = 0; epoch < kWorkgroupCounts.size(); ++epoch) {
    const uint32_t offset = kTupleByteOffsets[epoch] / sizeof(uint32_t);
    expected_tuples[offset] = kWorkgroupCounts[epoch];
    expected_tuples[offset + 1] = 1;
    expected_tuples[offset + 2] = 1;
  }
  // Both tuples stay immutable from first publication through final use.
  std::memcpy(tuples->host.pointer, expected_tuples.data(),
              sizeof(expected_tuples));
  expected_control[kCompletionWordIndex] = 0;
  std::memcpy(completion->host.pointer, expected_control.data(),
              sizeof(expected_control));
  const uintptr_t completion_address =
      reinterpret_cast<uintptr_t>(completion->host.pointer) +
      kCompletionByteOffset;

  GpuUserQueue* queue = nullptr;
  ASSERT_NO_FATAL_FAILURE(CreateQueue(&queue));
  const uint64_t ring_capacity =
      queue->host.ring_byte_length / sizeof(uint32_t);
  ASSERT_GT(ring_capacity, kWorkgroupCounts.size() * kCommandWordsPerEpoch);
  Pm4CommandWriter commands(
      reinterpret_cast<uint32_t*>(queue->host.ring_address));
  RecordProperty("pm4_indirect_packet_header", "0xc0021602");
  RecordProperty("pm4_indirect_dispatch_initiator", "0x8005");
  RecordProperty("pm4_indirect_count_units", "workgroups");
  RecordProperty("pm4_indirect_workgroup_size", "64x1x1");
  RecordProperty("pm4_indirect_tuple_byte_offsets", "256,320");
  RecordProperty("pm4_indirect_workgroup_count_sequence", "16x1x1,9x1x1");
  RecordProperty("pm4_indirect_active_count_sequence", "1024,576");
  RecordProperty("pm4_indirect_kernel_count", kCount);
  RecordProperty("pm4_indirect_payload_offset_words", kPayloadOffset);
  RecordProperty("pm4_indirect_checked_data_bytes_each",
                 sizeof(expected_input));
  RecordProperty("pm4_indirect_checked_page_bytes_each", kPageByteLength);
  RecordProperty("pm4_indirect_completion_byte_offset", kCompletionByteOffset);
  RecordProperty("pm4_indirect_command_words_per_epoch", kCommandWordsPerEpoch);
  RecordProperty("pm4_indirect_first_producer_index",
                 std::to_string(commands.word_count()));
  RecordProperty("pm4_indirect_ring_capacity_dwords",
                 std::to_string(ring_capacity));
  RecordProperty("pm4_indirect_completed_epochs", 0);

  for (uint32_t epoch = 0; epoch < kWorkgroupCounts.size(); ++epoch) {
    SCOPED_TRACE(epoch);
    for (uint32_t i = 0; i < kWordCount; ++i) {
      expected_input[i] = 0x759bf13du ^ i;
      expected_output[i] = 0x4e90b725u ^ i;
    }
    observed_output = expected_output;
    for (uint32_t i = 0; i < kCount; ++i) {
      const uint32_t value = static_cast<uint32_t>(
          UINT64_C(0xfffffff0) + uint64_t{i} * 0x01030507u +
          uint64_t{epoch} * 0x11111111u);
      const uint32_t result =
          static_cast<uint32_t>(uint64_t{value} * 3 + kAddends[epoch]);
      expected_input[kPayloadOffset + i] = value;
      // Poison every count-permitted lane, including the unlaunched tail.
      // Selecting the other immutable tuple changes 448 observed words.
      observed_output[kPayloadOffset + i] = ~result;
      expected_output[kPayloadOffset + i] =
          i < kActiveCounts[epoch] ? result : ~result;
    }
    std::memcpy(input->host.pointer, expected_input.data(),
                sizeof(expected_input));
    std::memcpy(output->host.pointer, observed_output.data(),
                sizeof(observed_output));
    const kernels::transform::Arguments payload = {
        input->device_address + kPayloadOffset * sizeof(uint32_t),
        output->device_address + kPayloadOffset * sizeof(uint32_t), kCount,
        kAddends[epoch]};
    expected_arguments.fill(0x3d);
    // Initialize host alignment padding without treating it as shader input.
    std::memset(expected_arguments.data(), 0, sizeof(payload));
    std::memcpy(expected_arguments.data(), &payload,
                kernel::kKernargByteLength);
    std::memcpy(arguments->host.pointer, expected_arguments.data(),
                sizeof(expected_arguments));
    expected_control[kCompletionWordIndex] = epoch + 1;

    commands.SystemBarrier();
    commands.BindCompute(program, arguments->device_address);
    commands.DispatchIndirectWave32(tuples->device_address +
                                    kTupleByteOffsets[epoch]);
    commands.SystemBarrier();
    commands.WriteData32(completion->device_address + kCompletionByteOffset,
                         epoch + 1);
    // The 55-word body needs a legal nine-word NOP, not a one-word packet.
    commands.PadToEightWords();
    ASSERT_EQ(commands.word_count(), (epoch + 1) * kCommandWordsPerEpoch);
    ASSERT_NO_FATAL_FAILURE(queue->PublishStream(commands.word_count()));
    GpuWaitEqual<uint32_t>(completion_address, epoch + 1);

    // Observe all initialized storage before diagnostics or ring retirement.
    std::memcpy(observed_output.data(), output->host.pointer,
                sizeof(observed_output));
    std::memcpy(observed_input.data(), input->host.pointer,
                sizeof(observed_input));
    std::memcpy(observed_arguments.data(), arguments->host.pointer,
                sizeof(observed_arguments));
    std::memcpy(observed_tuples.data(), tuples->host.pointer,
                sizeof(observed_tuples));
    std::memcpy(observed_control.data(), completion->host.pointer,
                sizeof(observed_control));
    std::memcpy(observed_code.data(), code->host.pointer,
                sizeof(observed_code));
    for (uint32_t i = 0; i < kWordCount; ++i) {
      EXPECT_EQ(observed_output[i], expected_output[i]) << "output word=" << i;
      EXPECT_EQ(observed_input[i], expected_input[i]) << "input word=" << i;
    }
    for (uint32_t i = 0; i < kPageByteLength; ++i) {
      EXPECT_EQ(observed_arguments[i], expected_arguments[i])
          << "argument byte=" << i;
      EXPECT_EQ(observed_code[i], expected_code[i]) << "code byte=" << i;
    }
    for (uint32_t i = 0; i < kPageWordCount; ++i) {
      EXPECT_EQ(observed_tuples[i], expected_tuples[i]) << "tuple word=" << i;
      EXPECT_EQ(observed_control[i], expected_control[i])
          << "control word=" << i;
    }
    EXPECT_NO_FATAL_FAILURE(queue->WaitConsumed(api_, commands.word_count()));
    if (HasFailure()) {
      return;
    }
    const std::string prefix =
        "pm4_indirect_epoch_" + std::to_string(epoch + 1);
    RecordProperty(prefix + "_addend", std::to_string(kAddends[epoch]));
    RecordProperty(prefix + "_producer_index",
                   std::to_string(commands.word_count()));
    RecordProperty("pm4_indirect_completed_epochs", epoch + 1);
  }
  RecordProperty("pm4_indirect_final_producer_index",
                 std::to_string(commands.word_count()));
}

}  // namespace
