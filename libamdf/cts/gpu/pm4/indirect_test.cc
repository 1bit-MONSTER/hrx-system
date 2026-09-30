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
#include "libamdf/cts/gpu/kernels/transform_kernels.h"
#include "libamdf/cts/gpu/pm4/dispatch_fixture.h"

namespace {

TEST_F(Pm4DispatchTest, SelectsImmutableIndirectWorkgroupCounts) {
  const auto* kernel_product =
      kernels::transform::kKernels.Find(gpu_endpoint_info_);
  ASSERT_NE(kernel_product, nullptr)
      << "missing compiled transform kernel for endpoint";
  const auto& kernel = *kernel_product;
  RecordProperty("transform_kernel_target", kernel.target);

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
  ASSERT_EQ(arguments->device_address % kernel.arguments.alignment, 0u);
  ASSERT_EQ(tuples->device_address % sizeof(uint32_t), 0u);
  ASSERT_EQ(completion->device_address % sizeof(uint32_t), 0u);
  Pm4ComputeProgram program = {0,
                               kernel.program.resource1,
                               kernel.program.resource2,
                               kernel.program.resource3,
                               kernel.group_segment_byte_length,
                               {kernel.workgroup_size(), 1, 1}};
  ASSERT_NO_FATAL_FAILURE(PrepareProgram(kernel.executable,
                                         kernel.entry_byte_offset, &program,
                                         "pm4_indirect", &code));

  std::array<uint32_t, kWordCount> expected_input, observed_input;
  std::array<uint32_t, kWordCount> expected_output, observed_output;
  std::array<uint8_t, kPageByteLength> expected_arguments, observed_arguments;
  std::array<uint32_t, kPageWordCount> expected_tuples, observed_tuples;
  std::array<uint32_t, kPageWordCount> expected_control, observed_control;
  std::array<uint8_t, kPageByteLength> expected_code = {};
  std::array<uint8_t, kPageByteLength> observed_code;
  std::memcpy(expected_code.data(), kernel.executable.words,
              kernel.executable.byte_length);
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

  GpuCommandQueue* queue = nullptr;
  ASSERT_NO_FATAL_FAILURE(CreateQueue(&queue));
  const uint64_t command_capacity = queue->words().size();
  ASSERT_GT(command_capacity, kWorkgroupCounts.size() * kCommandWordsPerEpoch);
  Pm4CommandWriter commands(queue->words().data(), *pm4_profile_);
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
  RecordProperty("pm4_indirect_first_published_word_count",
                 std::to_string(commands.word_count()));
  RecordProperty("pm4_indirect_command_capacity_dwords",
                 std::to_string(command_capacity));
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
                kernel.arguments.byte_length);
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
    ASSERT_NO_FATAL_FAILURE(
        queue->Publish(api_, gpu_api_, commands.word_count()));
    GpuWaitEqual<uint32_t>(completion_address, epoch + 1);

    // Observe all initialized storage before diagnostics or command storage
    // retirement.
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
    EXPECT_NO_FATAL_FAILURE(queue->WaitRetired(api_));
    if (HasFailure()) {
      return;
    }
    const std::string prefix =
        "pm4_indirect_epoch_" + std::to_string(epoch + 1);
    RecordProperty(prefix + "_addend", std::to_string(kAddends[epoch]));
    RecordProperty(prefix + "_published_word_count",
                   std::to_string(commands.word_count()));
    RecordProperty("pm4_indirect_completed_epochs", epoch + 1);
  }
  RecordProperty("pm4_indirect_final_published_word_count",
                 std::to_string(commands.word_count()));
}

TEST_F(Pm4DispatchTest, ShaderProducedCountsControlIndirectDispatch) {
  const auto* kernel_product =
      kernels::transform::kKernels.Find(gpu_endpoint_info_);
  ASSERT_NE(kernel_product, nullptr)
      << "missing compiled transform kernel for endpoint";
  const auto& kernel = *kernel_product;
  RecordProperty("transform_kernel_target", kernel.target);

  constexpr uint32_t kWordCount = 2048;
  constexpr uint32_t kPayloadOffset = 16;
  constexpr uint32_t kConsumerCount = 1536;
  constexpr uint32_t kProducerCount = 3;
  constexpr uint32_t kProducerInputByteOffset = 6656;
  constexpr uint32_t kProducerInputWordIndex =
      kProducerInputByteOffset / sizeof(uint32_t);
  constexpr uint32_t kPageByteLength = 4096;
  constexpr uint32_t kPageWordCount = kPageByteLength / sizeof(uint32_t);
  constexpr uint32_t kConsumerArgumentByteOffset = 64;
  constexpr uint32_t kTupleByteOffset = 256;
  constexpr uint32_t kTupleWordIndex = kTupleByteOffset / sizeof(uint32_t);
  constexpr uint32_t kCompletionByteOffset = 256;
  constexpr uint32_t kCompletionWordIndex =
      kCompletionByteOffset / sizeof(uint32_t);
  constexpr uint32_t kProducerAddend = 1;
  constexpr uint32_t kCommandWordsPerEpoch = 104;
  constexpr std::array<std::array<uint32_t, kProducerCount>, 2>
      kProducerInputs = {{{5, 0, 0}, {0x55555558u, 0, 0}}};
  constexpr std::array<uint32_t, 2> kWorkgroupCounts = {16, 9};
  constexpr std::array<uint32_t, 2> kActiveCounts = {1024, 576};
  constexpr std::array<uint32_t, 2> kConsumerAddends = {7, 0x80000023u};
  static_assert(kPayloadOffset + kConsumerCount <= kProducerInputWordIndex);
  static_assert(kProducerInputWordIndex + kProducerCount <= kWordCount);

  GpuMemory* input = nullptr;
  GpuMemory* output = nullptr;
  GpuMemory* arguments = nullptr;
  GpuMemory* tuple = nullptr;
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
      CreateMemory(AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE,
                   kPageByteLength, &tuple));
  ASSERT_NO_FATAL_FAILURE(
      CreateMemory(AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE,
                   kPageByteLength, &completion));
  ASSERT_EQ(arguments->device_address % kernel.arguments.alignment, 0u);
  ASSERT_EQ(tuple->device_address % sizeof(uint32_t), 0u);
  ASSERT_EQ(completion->device_address % sizeof(uint32_t), 0u);
  Pm4ComputeProgram program = {0,
                               kernel.program.resource1,
                               kernel.program.resource2,
                               kernel.program.resource3,
                               kernel.group_segment_byte_length,
                               {kernel.workgroup_size(), 1, 1}};
  ASSERT_NO_FATAL_FAILURE(PrepareProgram(kernel.executable,
                                         kernel.entry_byte_offset, &program,
                                         "pm4_produced_indirect", &code));

  std::array<uint32_t, kWordCount> expected_input, observed_input;
  std::array<uint32_t, kWordCount> expected_output, observed_output;
  std::array<uint8_t, kPageByteLength> expected_arguments, observed_arguments;
  std::array<uint32_t, kPageWordCount> expected_tuple, observed_tuple;
  std::array<uint32_t, kPageWordCount> expected_control, observed_control;
  std::array<uint8_t, kPageByteLength> expected_code = {};
  std::array<uint8_t, kPageByteLength> observed_code;
  std::memcpy(expected_code.data(), kernel.executable.words,
              kernel.executable.byte_length);
  for (uint32_t i = 0; i < kPageWordCount; ++i) {
    expected_tuple[i] = 0x9137ace5u ^ i;
    expected_control[i] = 0x68d329b7u ^ i;
  }
  // The host initializes the tuple once. Both its initial and prior-epoch
  // values describe valid bounded launches; only the producer changes it.
  expected_tuple[kTupleWordIndex] = 9;
  expected_tuple[kTupleWordIndex + 1] = 1;
  expected_tuple[kTupleWordIndex + 2] = 1;
  std::memcpy(tuple->host.pointer, expected_tuple.data(),
              sizeof(expected_tuple));
  expected_control[kCompletionWordIndex] = 0;
  std::memcpy(completion->host.pointer, expected_control.data(),
              sizeof(expected_control));
  const uintptr_t completion_address =
      reinterpret_cast<uintptr_t>(completion->host.pointer) +
      kCompletionByteOffset;

  GpuCommandQueue* queue = nullptr;
  ASSERT_NO_FATAL_FAILURE(CreateQueue(&queue));
  const uint64_t command_capacity = queue->words().size();
  ASSERT_GT(command_capacity, kWorkgroupCounts.size() * kCommandWordsPerEpoch);
  Pm4CommandWriter commands(queue->words().data(), *pm4_profile_);
  RecordProperty("pm4_produced_indirect_packet_header", "0xc0021602");
  RecordProperty("pm4_produced_indirect_dispatch_initiator", "0x8005");
  RecordProperty("pm4_produced_indirect_count_units", "workgroups");
  RecordProperty("pm4_produced_indirect_workgroup_size", "64x1x1");
  RecordProperty("pm4_produced_indirect_producer_grid_size", "64x1x1");
  RecordProperty("pm4_produced_indirect_producer_kernel_count", kProducerCount);
  RecordProperty("pm4_produced_indirect_producer_addend", kProducerAddend);
  RecordProperty("pm4_produced_indirect_producer_input_byte_offset",
                 kProducerInputByteOffset);
  RecordProperty("pm4_produced_indirect_producer_input_word_sequence",
                 "5,0,0;1431655768,0,0");
  RecordProperty("pm4_produced_indirect_tuple_byte_offset", kTupleByteOffset);
  RecordProperty("pm4_produced_indirect_initial_workgroup_counts", "9x1x1");
  RecordProperty("pm4_produced_indirect_workgroup_count_sequence",
                 "16x1x1,9x1x1");
  RecordProperty("pm4_produced_indirect_active_count_sequence", "1024,576");
  RecordProperty("pm4_produced_indirect_consumer_kernel_count", kConsumerCount);
  RecordProperty("pm4_produced_indirect_consumer_payload_byte_offset",
                 kPayloadOffset * sizeof(uint32_t));
  RecordProperty("pm4_produced_indirect_argument_byte_offsets", "0,64");
  RecordProperty("pm4_produced_indirect_argument_slot_bytes",
                 sizeof(kernels::transform::Arguments));
  RecordProperty("pm4_produced_indirect_argument_semantic_bytes",
                 kernel.arguments.byte_length);
  RecordProperty("pm4_produced_indirect_checked_data_bytes_each",
                 sizeof(expected_input));
  RecordProperty("pm4_produced_indirect_checked_page_bytes_each",
                 kPageByteLength);
  RecordProperty("pm4_produced_indirect_checked_total_bytes",
                 2 * sizeof(expected_input) + 4 * kPageByteLength);
  RecordProperty("pm4_produced_indirect_completion_byte_offset",
                 kCompletionByteOffset);
  RecordProperty("pm4_produced_indirect_command_words_per_epoch",
                 kCommandWordsPerEpoch);
  RecordProperty("pm4_produced_indirect_first_published_word_count",
                 std::to_string(commands.word_count()));
  RecordProperty("pm4_produced_indirect_command_capacity_dwords",
                 std::to_string(command_capacity));
  RecordProperty("pm4_produced_indirect_completed_epochs", 0);
  RecordProperty("pm4_produced_indirect_completed_dispatches", 0);

  for (uint32_t epoch = 0; epoch < kWorkgroupCounts.size(); ++epoch) {
    SCOPED_TRACE(epoch);
    for (uint32_t i = 0; i < kWordCount; ++i) {
      expected_input[i] = 0x759bf13du ^ i;
      expected_output[i] = 0x4e90b725u ^ i;
    }
    observed_output = expected_output;
    for (uint32_t i = 0; i < kConsumerCount; ++i) {
      const uint32_t value = static_cast<uint32_t>(
          UINT64_C(0xfffffff0) + uint64_t{i} * 0x01030507u +
          uint64_t{epoch} * 0x11111111u);
      const uint32_t result =
          static_cast<uint32_t>(uint64_t{value} * 3 + kConsumerAddends[epoch]);
      expected_input[kPayloadOffset + i] = value;
      // Every count-permitted lane starts as complement poison. A stale
      // 9/16-group tuple changes the expected state of 448 output words.
      observed_output[kPayloadOffset + i] = ~result;
      expected_output[kPayloadOffset + i] =
          i < kActiveCounts[epoch] ? result : ~result;
    }
    for (uint32_t i = 0; i < kProducerCount; ++i) {
      expected_input[kProducerInputWordIndex + i] = kProducerInputs[epoch][i];
    }
    std::memcpy(input->host.pointer, expected_input.data(),
                sizeof(expected_input));
    std::memcpy(output->host.pointer, observed_output.data(),
                sizeof(observed_output));
    const kernels::transform::Arguments producer_payload = {
        input->device_address + kProducerInputByteOffset,
        tuple->device_address + kTupleByteOffset, kProducerCount,
        kProducerAddend};
    const kernels::transform::Arguments consumer_payload = {
        input->device_address + kPayloadOffset * sizeof(uint32_t),
        output->device_address + kPayloadOffset * sizeof(uint32_t),
        kConsumerCount, kConsumerAddends[epoch]};
    expected_arguments.fill(0x3d);
    // Both argument records remain immutable through completion and
    // retirement. Zero the backing padding; copy only the 24 semantic bytes.
    std::memset(expected_arguments.data(), 0, sizeof(producer_payload));
    std::memcpy(expected_arguments.data(), &producer_payload,
                kernel.arguments.byte_length);
    std::memset(expected_arguments.data() + kConsumerArgumentByteOffset, 0,
                sizeof(consumer_payload));
    std::memcpy(expected_arguments.data() + kConsumerArgumentByteOffset,
                &consumer_payload, kernel.arguments.byte_length);
    std::memcpy(arguments->host.pointer, expected_arguments.data(),
                sizeof(expected_arguments));
    // These expected values never write the GPU tuple or completion storage.
    expected_tuple[kTupleWordIndex] = kWorkgroupCounts[epoch];
    expected_control[kCompletionWordIndex] = epoch + 1;

    commands.SystemBarrier();
    commands.BindCompute(program, arguments->device_address);
    // A complete workgroup executes; the shader's explicit count bounds its
    // three stores. This does not rely on hardware partial-group masking.
    commands.DispatchWave32(kernel.workgroup_size(), 1, 1);
    // Complete and publish the shader stores before the MEC fetches counts.
    commands.SystemBarrier();
    commands.BindCompute(
        program, arguments->device_address + kConsumerArgumentByteOffset);
    commands.DispatchIndirectWave32(tuple->device_address + kTupleByteOffset);
    // This independent terminal drain joins both dispatches before host
    // observation, even if the middle edge yields the wrong finite result.
    commands.SystemBarrier();
    commands.WriteData32(completion->device_address + kCompletionByteOffset,
                         epoch + 1);
    // The aligned 96-word body still receives a complete eight-word NOP.
    commands.PadToEightWords();
    ASSERT_EQ(commands.word_count(), (epoch + 1) * kCommandWordsPerEpoch);
    ASSERT_NO_FATAL_FAILURE(
        queue->Publish(api_, gpu_api_, commands.word_count()));
    GpuWaitEqual<uint32_t>(completion_address, epoch + 1);

    // Snapshot all six initialized allocations before diagnostics or
    // command storage retirement can add synchronization to the payload
    // observations.
    std::memcpy(observed_output.data(), output->host.pointer,
                sizeof(observed_output));
    std::memcpy(observed_tuple.data(), tuple->host.pointer,
                sizeof(observed_tuple));
    std::memcpy(observed_input.data(), input->host.pointer,
                sizeof(observed_input));
    std::memcpy(observed_arguments.data(), arguments->host.pointer,
                sizeof(observed_arguments));
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
      EXPECT_EQ(observed_tuple[i], expected_tuple[i]) << "tuple word=" << i;
      EXPECT_EQ(observed_control[i], expected_control[i])
          << "control word=" << i;
    }
    EXPECT_NO_FATAL_FAILURE(queue->WaitRetired(api_));
    if (HasFailure()) {
      return;
    }
    const std::string prefix =
        "pm4_produced_indirect_epoch_" + std::to_string(epoch + 1);
    RecordProperty(prefix + "_consumer_addend",
                   std::to_string(kConsumerAddends[epoch]));
    RecordProperty(prefix + "_observed_workgroup_count_x",
                   std::to_string(observed_tuple[kTupleWordIndex]));
    RecordProperty(prefix + "_published_word_count",
                   std::to_string(commands.word_count()));
    RecordProperty("pm4_produced_indirect_completed_epochs", epoch + 1);
    RecordProperty("pm4_produced_indirect_completed_dispatches",
                   (epoch + 1) * 2);
  }
  RecordProperty("pm4_produced_indirect_final_published_word_count",
                 std::to_string(commands.word_count()));
}

}  // namespace
