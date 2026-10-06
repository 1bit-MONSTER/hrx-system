// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <array>
#include <cstdint>
#include <cstring>
#include <string>

#include "libamdf/cts/gpu/kernels/transform.h"
#include "libamdf/cts/gpu/kernels/transform_kernels.h"
#include "libamdf/cts/gpu/pm4/dispatch_fixture.h"
#include "libamdf/cts/gpu/pm4/encoding/commands.h"

namespace {

TEST_F(Pm4DispatchTest, CoherentSystemPayloadChangesAcrossEpochs) {
  const auto* kernel_product =
      kernels::transform::kKernels.Find(gpu_endpoint_info_);
  ASSERT_NE(kernel_product, nullptr)
      << "missing compiled transform kernel for endpoint";
  const auto& kernel = *kernel_product;
  RecordProperty("transform_kernel_target", kernel.target);

  constexpr uint32_t kGridSize = 1024;
  constexpr uint32_t kWordCount = 2048;
  constexpr uint32_t kPayloadOffset = 16;
  constexpr uint32_t kGuard = 0x759bf13du;
  constexpr std::array<uint32_t, 2> kCounts = {1003, 997};
  constexpr std::array<uint32_t, 2> kAddends = {7, 0x80000023u};
  GpuMemory* input = nullptr;
  GpuMemory* output = nullptr;
  GpuMemory* arguments = nullptr;
  GpuMemory* completion = nullptr;
  ASSERT_NO_FATAL_FAILURE(CreateMemory(AMDF_MEMORY_ACCESS_READ,
                                       kWordCount * sizeof(uint32_t), &input));
  ASSERT_NO_FATAL_FAILURE(
      CreateMemory(AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE,
                   kWordCount * sizeof(uint32_t), &output));
  ASSERT_NO_FATAL_FAILURE(
      CreateMemory(AMDF_MEMORY_ACCESS_READ, 4096, &arguments));
  ASSERT_NO_FATAL_FAILURE(CreateMemory(
      AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE, 4096, &completion));
  ASSERT_EQ(arguments->device_address % kernel.arguments.alignment, 0u);
  std::memset(completion->host.pointer, 0, completion->info.byte_length);
  Pm4ComputeProgram program = {
      0,
      kernel.program.resource1,
      kernel.program.resource2,
      kernel.program.resource3,
      kernel.group_segment_byte_length,
      {kernel.workgroup_size(), 1, 1},
  };
  ASSERT_NO_FATAL_FAILURE(PrepareProgram(
      kernel.executable, kernel.entry_byte_offset, &program, "pm4"));

  GpuCommandQueue* queue = nullptr;
  ASSERT_NO_FATAL_FAILURE(CreateQueue(&queue));
  // Each batch has 56 command words and an eight-word NOP. Both batches occupy
  // distinct command ranges and are published separately.
  ASSERT_GE(queue->words().size_bytes(), 128 * sizeof(uint32_t));
  Pm4CommandWriter commands(queue->words().data(), *pm4_profile_);
  for (uint32_t epoch = 0; epoch < kCounts.size(); ++epoch) {
    std::array<uint32_t, kWordCount> upload;
    std::array<uint32_t, kWordCount> expected;
    std::array<uint32_t, kWordCount> download;
    upload.fill(kGuard);
    expected.fill(kGuard);
    download.fill(kGuard);
    for (uint32_t i = 0; i < kGridSize; ++i) {
      const uint32_t value =
          0xfffffff0u + i * 0x01030507u + epoch * 0x11111111u;
      upload[kPayloadOffset + i] = value;
      if (i < kCounts[epoch]) {
        // The CPU oracle uses wider arithmetic, then applies uint32 wrapping.
        const uint32_t result =
            static_cast<uint32_t>(uint64_t{value} * 3 + kAddends[epoch]);
        expected[kPayloadOffset + i] = result;
        download[kPayloadOffset + i] = ~result;
      }
    }
    std::memcpy(input->host.pointer, upload.data(), sizeof(upload));
    std::memcpy(output->host.pointer, download.data(), sizeof(download));
    const kernels::transform::Arguments payload = {
        input->device_address + kPayloadOffset * sizeof(uint32_t),
        output->device_address + kPayloadOffset * sizeof(uint32_t),
        kCounts[epoch],
        kAddends[epoch],
    };
    std::memset(arguments->host.pointer, 0, arguments->info.byte_length);
    std::memcpy(arguments->host.pointer, &payload,
                kernel.arguments.byte_length);

    commands.SystemBarrier();
    commands.BindCompute(program, arguments->device_address);
    commands.DispatchWave32(kGridSize, 1, 1);
    commands.SystemBarrier();
    // Monotonic completion values prevent a prior epoch from satisfying this
    // wait; the host never resets a value that the command processor writes.
    commands.WriteData32(completion->device_address, epoch + 1);
    commands.PadToEightWords();
    ASSERT_NO_FATAL_FAILURE(
        queue->Publish(api_, gpu_api_, commands.word_count()));
    GpuWaitEqual<uint32_t>(
        reinterpret_cast<uintptr_t>(completion->host.pointer), epoch + 1);
    std::memcpy(download.data(), output->host.pointer, sizeof(download));
    const auto* unchanged_input =
        static_cast<const uint32_t*>(input->host.pointer);
    for (uint32_t i = 0; i < kWordCount; ++i) {
      EXPECT_EQ(download[i], expected[i]) << "epoch=" << epoch << " word=" << i;
      EXPECT_EQ(unchanged_input[i], upload[i])
          << "epoch=" << epoch << " word=" << i;
    }
    // Observe the complete payload before retirement can add synchronization.
    // Retire the stream even on an oracle failure, then stop before reuse.
    ASSERT_NO_FATAL_FAILURE(queue->WaitRetired(api_));
    if (HasFailure()) {
      return;
    }
  }
  RecordProperty("pm4_payload_completed_epochs", kCounts.size());
}

TEST_F(Pm4DispatchTest, CoherentSystemProducerConsumerChainAcrossEpochs) {
  const auto* kernel_product =
      kernels::transform::kKernels.Find(gpu_endpoint_info_);
  ASSERT_NE(kernel_product, nullptr)
      << "missing compiled transform kernel for endpoint";
  const auto& kernel = *kernel_product;
  RecordProperty("transform_kernel_target", kernel.target);

  constexpr uint32_t kGridSize = 1024;
  constexpr uint32_t kWordCount = 2048;
  constexpr uint32_t kPayloadOffset = 16;
  constexpr uint32_t kInputGuard = 0x759bf13du;
  constexpr uint32_t kIntermediateGuard = 0xa36cf197u;
  constexpr uint32_t kOutputGuard = 0x4e90b725u;
  constexpr uint32_t kControlGuard = 0x68d329b7u;
  constexpr uint32_t kControlWordCount = 1024;
  constexpr uint32_t kArgumentStride = 64;
  constexpr uint32_t kCommandWordCountPerEpoch = 104;
  constexpr std::array<uint32_t, 2> kCounts = {1003, 997};
  constexpr std::array<uint32_t, 2> kProducerAddends = {7, 0x80000023u};
  constexpr std::array<uint32_t, 2> kConsumerAddends = {11, 0x10203045u};

  GpuMemory* input = nullptr;
  GpuMemory* intermediate = nullptr;
  GpuMemory* output = nullptr;
  GpuMemory* arguments = nullptr;
  GpuMemory* completion = nullptr;
  ASSERT_NO_FATAL_FAILURE(CreateMemory(AMDF_MEMORY_ACCESS_READ,
                                       kWordCount * sizeof(uint32_t), &input));
  ASSERT_NO_FATAL_FAILURE(
      CreateMemory(AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE,
                   kWordCount * sizeof(uint32_t), &intermediate));
  ASSERT_NO_FATAL_FAILURE(
      CreateMemory(AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE,
                   kWordCount * sizeof(uint32_t), &output));
  ASSERT_NO_FATAL_FAILURE(
      CreateMemory(AMDF_MEMORY_ACCESS_READ, 4096, &arguments));
  ASSERT_NO_FATAL_FAILURE(
      CreateMemory(AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE,
                   kControlWordCount * sizeof(uint32_t), &completion));
  ASSERT_EQ(arguments->device_address % kernel.arguments.alignment, 0u);
  std::array<uint32_t, kControlWordCount> control_words;
  control_words.fill(kControlGuard);
  control_words[0] = 0;
  std::memcpy(completion->host.pointer, control_words.data(),
              sizeof(control_words));
  Pm4ComputeProgram program = {
      0,
      kernel.program.resource1,
      kernel.program.resource2,
      kernel.program.resource3,
      kernel.group_segment_byte_length,
      {kernel.workgroup_size(), 1, 1},
  };
  ASSERT_NO_FATAL_FAILURE(PrepareProgram(
      kernel.executable, kernel.entry_byte_offset, &program, "pm4"));

  GpuCommandQueue* queue = nullptr;
  ASSERT_NO_FATAL_FAILURE(CreateQueue(&queue));
  // Each epoch has 97 command words and a seven-word NOP. Both finite batches
  // remain in distinct resident command ranges.
  ASSERT_GE(queue->words().size(), kCommandWordCountPerEpoch * kCounts.size());
  Pm4CommandWriter commands(queue->words().data(), *pm4_profile_);
  for (uint32_t epoch = 0; epoch < kCounts.size(); ++epoch) {
    SCOPED_TRACE(epoch);
    std::array<uint32_t, kWordCount> upload;
    std::array<uint32_t, kWordCount> expected_intermediate;
    std::array<uint32_t, kWordCount> expected_output;
    std::array<uint32_t, kWordCount> input_words;
    std::array<uint32_t, kWordCount> intermediate_words;
    std::array<uint32_t, kWordCount> output_words;
    upload.fill(kInputGuard);
    expected_intermediate.fill(kIntermediateGuard);
    expected_output.fill(kOutputGuard);
    intermediate_words.fill(kIntermediateGuard);
    output_words.fill(kOutputGuard);
    for (uint32_t i = 0; i < kGridSize; ++i) {
      const uint32_t value =
          0xfffffff0u + i * 0x01030507u + epoch * 0x11111111u;
      upload[kPayloadOffset + i] = value;
      if (i < kCounts[epoch]) {
        const uint32_t intermediate_result = static_cast<uint32_t>(
            uint64_t{value} * 3 + kProducerAddends[epoch]);
        // Flatten both transforms with wide CPU arithmetic. Observed GPU
        // intermediate values never participate in the final output oracle.
        const uint32_t output_result = static_cast<uint32_t>(
            uint64_t{value} * 9 + uint64_t{kProducerAddends[epoch]} * 3 +
            kConsumerAddends[epoch]);
        expected_intermediate[kPayloadOffset + i] = intermediate_result;
        expected_output[kPayloadOffset + i] = output_result;
        intermediate_words[kPayloadOffset + i] = ~intermediate_result;
        output_words[kPayloadOffset + i] = ~output_result;
      }
    }
    std::memcpy(input->host.pointer, upload.data(), sizeof(upload));
    std::memcpy(intermediate->host.pointer, intermediate_words.data(),
                sizeof(intermediate_words));
    std::memcpy(output->host.pointer, output_words.data(),
                sizeof(output_words));
    const kernels::transform::Arguments producer_payload = {
        input->device_address + kPayloadOffset * sizeof(uint32_t),
        intermediate->device_address + kPayloadOffset * sizeof(uint32_t),
        kCounts[epoch],
        kProducerAddends[epoch],
    };
    const kernels::transform::Arguments consumer_payload = {
        intermediate->device_address + kPayloadOffset * sizeof(uint32_t),
        output->device_address + kPayloadOffset * sizeof(uint32_t),
        kCounts[epoch],
        kConsumerAddends[epoch],
    };
    // Both records retain zero padding and remain immutable through terminal
    // completion and retirement. The compiler consumes only 24 bytes each.
    std::memset(arguments->host.pointer, 0, arguments->info.byte_length);
    std::memcpy(arguments->host.pointer, &producer_payload,
                kernel.arguments.byte_length);
    std::memcpy(
        static_cast<uint8_t*>(arguments->host.pointer) + kArgumentStride,
        &consumer_payload, kernel.arguments.byte_length);

    commands.SystemBarrier();
    commands.BindCompute(program, arguments->device_address);
    commands.DispatchWave32(kGridSize, 1, 1);
    // The queue permits unordered dispatch. This explicit completion/cache
    // edge makes the producer's payload available before the consumer loads.
    commands.SystemBarrier();
    commands.BindCompute(program, arguments->device_address + kArgumentStride);
    commands.DispatchWave32(kGridSize, 1, 1);
    // Independently drain both dispatches before host observation, even if the
    // middle dependency produces incorrect data. The confirmed marker follows
    // the final cache operations and is never reset by the host.
    commands.SystemBarrier();
    commands.WriteData32(completion->device_address, epoch + 1);
    commands.PadToEightWords();
    ASSERT_EQ(commands.word_count(), (epoch + 1) * kCommandWordCountPerEpoch);
    ASSERT_NO_FATAL_FAILURE(
        queue->Publish(api_, gpu_api_, commands.word_count()));
    GpuWaitEqual<uint32_t>(
        reinterpret_cast<uintptr_t>(completion->host.pointer), epoch + 1);

    // Capture every observed byte before diagnostics or
    // command-retirement operations can add synchronization to the
    // payload observations.
    std::memcpy(output_words.data(), output->host.pointer,
                sizeof(output_words));
    std::memcpy(intermediate_words.data(), intermediate->host.pointer,
                sizeof(intermediate_words));
    std::memcpy(input_words.data(), input->host.pointer, sizeof(input_words));
    std::memcpy(control_words.data(), completion->host.pointer,
                sizeof(control_words));
    for (uint32_t i = 0; i < kWordCount; ++i) {
      EXPECT_EQ(output_words[i], expected_output[i]) << "output word=" << i;
      EXPECT_EQ(intermediate_words[i], expected_intermediate[i])
          << "intermediate word=" << i;
      EXPECT_EQ(input_words[i], upload[i]) << "input word=" << i;
    }
    for (uint32_t i = 0; i < kControlWordCount; ++i) {
      EXPECT_EQ(control_words[i], i == 0 ? epoch + 1 : kControlGuard)
          << "control word=" << i;
    }
    // Oracle failures still retire the complete stream. Neither backing nor
    // arguments may be reused after a failed observation or retirement.
    EXPECT_NO_FATAL_FAILURE(queue->WaitRetired(api_));
    if (HasFailure()) {
      return;
    }
  }
  RecordProperty("pm4_chain_completed_epochs", kCounts.size());
  RecordProperty("pm4_chain_command_word_count",
                 std::to_string(commands.word_count()));
}

TEST_F(Pm4DispatchTest, CoherentSystemReleaseCompletesShaderAcrossEpochs) {
  const auto* kernel_product =
      kernels::transform::kKernels.Find(gpu_endpoint_info_);
  ASSERT_NE(kernel_product, nullptr)
      << "missing compiled transform kernel for endpoint";
  const auto& kernel = *kernel_product;
  RecordProperty("transform_kernel_target", kernel.target);

  constexpr uint32_t kGridSize = 1024;
  constexpr uint32_t kWordCount = 2048;
  constexpr uint32_t kPayloadOffset = 16;
  constexpr uint32_t kInputGuard = 0x759bf13du;
  constexpr uint32_t kOutputGuard = 0x4e90b725u;
  constexpr uint32_t kControlGuard = 0x68d329b7u;
  constexpr uint32_t kControlWordCount = 1024;
  constexpr uint32_t kCompletionByteOffset = 256;
  constexpr uint32_t kCompletionWordIndex =
      kCompletionByteOffset / sizeof(uint32_t);
  constexpr uint32_t kCommandWordCountPerEpoch = 56;
  constexpr std::array<uint32_t, 2> kCounts = {1003, 997};
  constexpr std::array<uint32_t, 2> kAddends = {7, 0x80000023u};

  GpuMemory* input = nullptr;
  GpuMemory* output = nullptr;
  GpuMemory* arguments = nullptr;
  GpuMemory* completion = nullptr;
  ASSERT_NO_FATAL_FAILURE(CreateMemory(AMDF_MEMORY_ACCESS_READ,
                                       kWordCount * sizeof(uint32_t), &input));
  ASSERT_NO_FATAL_FAILURE(
      CreateMemory(AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE,
                   kWordCount * sizeof(uint32_t), &output));
  ASSERT_NO_FATAL_FAILURE(
      CreateMemory(AMDF_MEMORY_ACCESS_READ, 4096, &arguments));
  ASSERT_NO_FATAL_FAILURE(
      CreateMemory(AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE,
                   kControlWordCount * sizeof(uint32_t), &completion));
  ASSERT_EQ(arguments->device_address % kernel.arguments.alignment, 0u);
  ASSERT_EQ(completion->device_address % sizeof(uint32_t), 0u);
  std::array<uint32_t, kControlWordCount> control_words;
  control_words.fill(kControlGuard);
  control_words[kCompletionWordIndex] = 0;
  // Initialize the whole control page once. Only the GPU advances its epoch
  // word, so a prior completion cannot satisfy the next epoch's wait.
  std::memcpy(completion->host.pointer, control_words.data(),
              sizeof(control_words));
  auto* completion_word =
      static_cast<uint32_t*>(completion->host.pointer) + kCompletionWordIndex;
  Pm4ComputeProgram program = {
      0,
      kernel.program.resource1,
      kernel.program.resource2,
      kernel.program.resource3,
      kernel.group_segment_byte_length,
      {kernel.workgroup_size(), 1, 1},
  };
  ASSERT_NO_FATAL_FAILURE(PrepareProgram(
      kernel.executable, kernel.entry_byte_offset, &program, "pm4"));

  GpuCommandQueue* queue = nullptr;
  ASSERT_NO_FATAL_FAILURE(CreateQueue(&queue));
  // The 49-word sequence is followed by a seven-word NOP. Both batches remain
  // in distinct resident command ranges.
  ASSERT_GE(queue->words().size(), kCommandWordCountPerEpoch * kCounts.size());
  Pm4CommandWriter commands(queue->words().data(), *pm4_profile_);
  RecordProperty("pm4_release_completion_byte_offset", kCompletionByteOffset);
  RecordProperty("pm4_release_command_word_count_per_epoch",
                 kCommandWordCountPerEpoch);
  for (uint32_t epoch = 0; epoch < kCounts.size(); ++epoch) {
    SCOPED_TRACE(epoch);
    std::array<uint32_t, kWordCount> upload;
    std::array<uint32_t, kWordCount> expected_output;
    std::array<uint32_t, kWordCount> input_words;
    std::array<uint32_t, kWordCount> output_words;
    upload.fill(kInputGuard);
    expected_output.fill(kOutputGuard);
    output_words.fill(kOutputGuard);
    for (uint32_t i = 0; i < kGridSize; ++i) {
      const uint32_t value =
          0xfffffff0u + i * 0x01030507u + epoch * 0x11111111u;
      upload[kPayloadOffset + i] = value;
      if (i < kCounts[epoch]) {
        // Compute independently in wider arithmetic, then apply uint32 wrap.
        const uint32_t result =
            static_cast<uint32_t>(uint64_t{value} * 3 + kAddends[epoch]);
        expected_output[kPayloadOffset + i] = result;
        output_words[kPayloadOffset + i] = ~result;
      }
    }
    std::memcpy(input->host.pointer, upload.data(), sizeof(upload));
    std::memcpy(output->host.pointer, output_words.data(),
                sizeof(output_words));
    const kernels::transform::Arguments payload = {
        input->device_address + kPayloadOffset * sizeof(uint32_t),
        output->device_address + kPayloadOffset * sizeof(uint32_t),
        kCounts[epoch],
        kAddends[epoch],
    };
    std::memset(arguments->host.pointer, 0, arguments->info.byte_length);
    std::memcpy(arguments->host.pointer, &payload,
                kernel.arguments.byte_length);

    commands.SystemBarrier();
    commands.BindCompute(program, arguments->device_address);
    commands.DispatchWave32(kGridSize, 1, 1);
    // This bottom-of-pipe release publishes the shader's vector stores and
    // writes the known epoch. It is the sole payload-completion signal.
    commands.ReleaseSystem32(completion->device_address + kCompletionByteOffset,
                             epoch + 1);
    commands.PadToEightWords();
    ASSERT_EQ(commands.word_count(), (epoch + 1) * kCommandWordCountPerEpoch);
    ASSERT_NO_FATAL_FAILURE(
        queue->Publish(api_, gpu_api_, commands.word_count()));
    GpuWaitEqual<uint32_t>(reinterpret_cast<uintptr_t>(completion_word),
                           epoch + 1);

    // Snapshot every observed byte before diagnostics or retirement
    // polling can add synchronization to the payload observation.
    std::memcpy(output_words.data(), output->host.pointer,
                sizeof(output_words));
    std::memcpy(input_words.data(), input->host.pointer, sizeof(input_words));
    std::memcpy(control_words.data(), completion->host.pointer,
                sizeof(control_words));
    for (uint32_t i = 0; i < kWordCount; ++i) {
      EXPECT_EQ(output_words[i], expected_output[i]) << "output word=" << i;
      EXPECT_EQ(input_words[i], upload[i]) << "input word=" << i;
    }
    for (uint32_t i = 0; i < kControlWordCount; ++i) {
      EXPECT_EQ(control_words[i],
                i == kCompletionWordIndex ? epoch + 1 : kControlGuard)
          << "control word=" << i;
    }
    // Nonfatal oracle failures still reach retirement. No arguments or payload
    // are rewritten after a failed observation or retirement wait.
    EXPECT_NO_FATAL_FAILURE(queue->WaitRetired(api_));
    if (HasFailure()) {
      return;
    }
  }
  RecordProperty("pm4_release_completed_epochs", kCounts.size());
  RecordProperty("pm4_release_command_word_count",
                 std::to_string(commands.word_count()));
}

TEST_F(Pm4DispatchTest, CoherentSystemShaderTimestampsAcrossEpochs) {
  const auto* kernel_product =
      kernels::transform::kKernels.Find(gpu_endpoint_info_);
  ASSERT_NE(kernel_product, nullptr)
      << "missing compiled transform kernel for endpoint";
  const auto& kernel = *kernel_product;
  RecordProperty("transform_kernel_target", kernel.target);

  constexpr uint32_t kGridSize = 1024;
  constexpr uint32_t kWordCount = 2048;
  constexpr uint32_t kPayloadOffset = 16;
  constexpr uint32_t kInputGuard = 0x759bf13du;
  constexpr uint32_t kOutputGuard = 0x4e90b725u;
  constexpr uint32_t kControlGuard = 0x68d329b7u;
  constexpr uint32_t kControlWordCount = 1024;
  constexpr std::array<uint32_t, 2> kTimestampByteOffsets = {8, 72};
  constexpr std::array<uint64_t, 2> kTimestampPoisons = {UINT64_MAX, 0};
  constexpr uint32_t kFenceByteOffset = 128;
  constexpr uint32_t kEventByteOffset = 192;
  constexpr uint32_t kMarkerByteOffset = 256;
  constexpr uint32_t kCommandWordCountPerEpoch = 120;
  constexpr std::array<uint32_t, 2> kCounts = {1003, 997};
  constexpr std::array<uint32_t, 2> kAddends = {7, 0x80000023u};

  GpuMemory* input = nullptr;
  GpuMemory* output = nullptr;
  GpuMemory* arguments = nullptr;
  GpuMemory* control = nullptr;
  ASSERT_NO_FATAL_FAILURE(CreateMemory(AMDF_MEMORY_ACCESS_READ,
                                       kWordCount * sizeof(uint32_t), &input));
  ASSERT_NO_FATAL_FAILURE(
      CreateMemory(AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE,
                   kWordCount * sizeof(uint32_t), &output));
  ASSERT_NO_FATAL_FAILURE(
      CreateMemory(AMDF_MEMORY_ACCESS_READ, 4096, &arguments));
  ASSERT_NO_FATAL_FAILURE(
      CreateMemory(AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE,
                   kControlWordCount * sizeof(uint32_t), &control));
  ASSERT_EQ(arguments->device_address % kernel.arguments.alignment, 0u);
  ASSERT_EQ(control->device_address % sizeof(uint64_t), 0u);
  std::array<uint32_t, kControlWordCount> control_words;
  control_words.fill(kControlGuard);
  control_words[kFenceByteOffset / sizeof(uint32_t)] = 0;
  control_words[kEventByteOffset / sizeof(uint32_t)] = 0;
  control_words[kMarkerByteOffset / sizeof(uint32_t)] = 0;
  // Initialize all control storage once. Only the GPU advances the private
  // fence, session event and terminal marker after this initial publication.
  std::memcpy(control->host.pointer, control_words.data(),
              sizeof(control_words));
  auto* control_bytes = static_cast<uint8_t*>(control->host.pointer);
  const uintptr_t marker_host_address =
      reinterpret_cast<uintptr_t>(control_bytes + kMarkerByteOffset);
  Pm4ComputeProgram program = {
      0,
      kernel.program.resource1,
      kernel.program.resource2,
      kernel.program.resource3,
      kernel.group_segment_byte_length,
      {kernel.workgroup_size(), 1, 1},
  };
  ASSERT_NO_FATAL_FAILURE(PrepareProgram(
      kernel.executable, kernel.entry_byte_offset, &program, "pm4"));

  GpuCommandQueue* queue = nullptr;
  ASSERT_NO_FATAL_FAILURE(CreateQueue(&queue));
  // Each epoch has 114 active words and a six-word NOP. Both batches remain
  // resident in distinct ranges, leaving the USER ring's required free word.
  ASSERT_GT(queue->words().size(), kCommandWordCountPerEpoch * kCounts.size());
  Pm4CommandWriter commands(queue->words().data(), *pm4_profile_);
  RecordProperty("pm4_shader_timestamp_command_word_count_per_epoch",
                 kCommandWordCountPerEpoch);
  uint64_t previous_end_ticks = 0;
  for (uint32_t epoch = 0; epoch < kCounts.size(); ++epoch) {
    SCOPED_TRACE(epoch);
    std::array<uint32_t, kWordCount> upload;
    std::array<uint32_t, kWordCount> expected_output;
    std::array<uint32_t, kWordCount> input_words;
    std::array<uint32_t, kWordCount> output_words;
    upload.fill(kInputGuard);
    expected_output.fill(kOutputGuard);
    output_words.fill(kOutputGuard);
    for (uint32_t i = 0; i < kGridSize; ++i) {
      const uint32_t value =
          0xfffffff0u + i * 0x01030507u + epoch * 0x11111111u;
      upload[kPayloadOffset + i] = value;
      if (i < kCounts[epoch]) {
        // Compute independently in wider arithmetic, then apply uint32 wrap.
        const uint32_t result =
            static_cast<uint32_t>(uint64_t{value} * 3 + kAddends[epoch]);
        expected_output[kPayloadOffset + i] = result;
        output_words[kPayloadOffset + i] = ~result;
      }
    }
    std::memcpy(input->host.pointer, upload.data(), sizeof(upload));
    std::memcpy(output->host.pointer, output_words.data(),
                sizeof(output_words));
    const kernels::transform::Arguments payload = {
        input->device_address + kPayloadOffset * sizeof(uint32_t),
        output->device_address + kPayloadOffset * sizeof(uint32_t),
        kCounts[epoch],
        kAddends[epoch],
    };
    std::memset(arguments->host.pointer, 0, arguments->info.byte_length);
    std::memcpy(arguments->host.pointer, &payload,
                kernel.arguments.byte_length);
    // Re-poison only the two retired timestamp slots. Fence/event/marker words
    // retain their GPU-written values; no prior value satisfies a later wait.
    for (size_t i = 0; i < kTimestampByteOffsets.size(); ++i) {
      std::memcpy(control_bytes + kTimestampByteOffsets[i],
                  &kTimestampPoisons[i], sizeof(uint64_t));
    }

    commands.SystemBarrier();
    commands.BindCompute(program, arguments->device_address);
    commands.CopyGpuClock64(control->device_address + kTimestampByteOffsets[0]);
    commands.DispatchWave32(kGridSize, 1, 1);
    commands.ReleaseGpuClock64(control->device_address +
                               kTimestampByteOffsets[1]);
    // Join the timestamp release and publish its and the shader's stores.
    commands.WaitEndOfPipeAndWriteback(
        control->device_address + kFenceByteOffset, epoch * 2 + 1);
    commands.Release32(control->device_address + kEventByteOffset, epoch + 1);
    // Work follows the session event. Preserve its second join/writeback
    // before the terminal CP marker makes host observation safe.
    commands.WaitEndOfPipeAndWriteback(
        control->device_address + kFenceByteOffset, epoch * 2 + 2);
    commands.WriteData32(control->device_address + kMarkerByteOffset,
                         epoch + 1);
    commands.PadToEightWords();
    ASSERT_EQ(commands.word_count(), (epoch + 1) * kCommandWordCountPerEpoch);
    ASSERT_NO_FATAL_FAILURE(
        queue->Publish(api_, gpu_api_, commands.word_count()));
    GpuWaitEqual<uint32_t>(marker_host_address, epoch + 1);

    // Capture every observed byte before diagnostics or retirement polling
    // can add synchronization. The marker is the only host completion wait.
    std::memcpy(output_words.data(), output->host.pointer,
                sizeof(output_words));
    std::memcpy(input_words.data(), input->host.pointer, sizeof(input_words));
    std::memcpy(control_words.data(), control->host.pointer,
                sizeof(control_words));
    std::array<uint64_t, 2> samples;
    for (size_t i = 0; i < samples.size(); ++i) {
      std::memcpy(&samples[i],
                  reinterpret_cast<const uint8_t*>(control_words.data()) +
                      kTimestampByteOffsets[i],
                  sizeof(uint64_t));
    }
    for (uint32_t i = 0; i < kWordCount; ++i) {
      EXPECT_EQ(output_words[i], expected_output[i]) << "output word=" << i;
      EXPECT_EQ(input_words[i], upload[i]) << "input word=" << i;
    }
    for (uint32_t i = 0; i < kControlWordCount; ++i) {
      const uint32_t byte_offset = i * sizeof(uint32_t);
      if ((byte_offset >= kTimestampByteOffsets[0] &&
           byte_offset < kTimestampByteOffsets[0] + sizeof(uint64_t)) ||
          (byte_offset >= kTimestampByteOffsets[1] &&
           byte_offset < kTimestampByteOffsets[1] + sizeof(uint64_t))) {
        continue;
      }
      uint32_t expected = kControlGuard;
      if (byte_offset == kFenceByteOffset) {
        expected = epoch * 2 + 2;
      } else if (byte_offset == kEventByteOffset ||
                 byte_offset == kMarkerByteOffset) {
        expected = epoch + 1;
      }
      EXPECT_EQ(control_words[i], expected) << "control word=" << i;
    }
    EXPECT_NE(samples[0], kTimestampPoisons[0]);
    EXPECT_NE(samples[1], kTimestampPoisons[1]);
    // Equal raw samples are legal. This finite run assumes no counter wrap or
    // reset and makes no tick-frequency, atomicity or elapsed-time claim.
    EXPECT_LE(samples[0], samples[1]);
    if (epoch != 0) {
      EXPECT_LE(previous_end_ticks, samples[0]);
    }
    const std::string property_prefix =
        "pm4_shader_timestamp_epoch_" + std::to_string(epoch);
    RecordProperty(property_prefix + "_begin_ticks",
                   std::to_string(samples[0]));
    RecordProperty(property_prefix + "_end_ticks", std::to_string(samples[1]));
    RecordProperty(property_prefix + "_fence_value",
                   control_words[kFenceByteOffset / sizeof(uint32_t)]);
    RecordProperty(property_prefix + "_event_value",
                   control_words[kEventByteOffset / sizeof(uint32_t)]);
    RecordProperty(property_prefix + "_marker_value",
                   control_words[kMarkerByteOffset / sizeof(uint32_t)]);
    RecordProperty(property_prefix + "_command_word_count",
                   std::to_string(commands.word_count()));
    // Retire trailing command storage even after a nonfatal oracle failure.
    // No timestamp, arguments or payload may be reused after either failure.
    EXPECT_NO_FATAL_FAILURE(queue->WaitRetired(api_));
    if (HasFailure()) {
      return;
    }
    previous_end_ticks = samples[1];
  }
  RecordProperty("pm4_shader_timestamp_completed_epochs", kCounts.size());
  RecordProperty("pm4_shader_timestamp_command_word_count",
                 std::to_string(commands.word_count()));
}

}  // namespace
