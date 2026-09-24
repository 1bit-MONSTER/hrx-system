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
#include <vector>

#include "libamdf/cts/gpu/kernels/transform.h"
#include "libamdf/cts/gpu/kernels/transform_gfx1151.h"
#include "libamdf/cts/gpu/pm4/dispatch_fixture.h"
#include "libamdf/cts/gpu/pm4/encoding/commands.h"

namespace {

namespace kernel = kernels::gfx1151_transform;

TEST_F(Pm4DispatchTest, ExecutesImmutableIndirectBufferAcrossEpochs) {
  constexpr uint32_t kGridSize = 1024;
  constexpr uint32_t kWordCount = 2048;
  constexpr uint32_t kPayloadOffset = 16;
  constexpr uint32_t kPageByteLength = 4096;
  constexpr uint32_t kPageWordCount = kPageByteLength / sizeof(uint32_t);
  constexpr uint32_t kIndirectWordCount = 40;
  constexpr uint32_t kRingWordsPerEpoch = 32;
  constexpr uint32_t kCompletionWord = 0;
  constexpr std::array<uint32_t, 2> kCounts = {1003, 997};
  constexpr std::array<uint32_t, 2> kAddends = {7, 0x80000023u};
  static_assert(offsetof(kernels::transform::Arguments, addend) +
                    sizeof(uint32_t) ==
                kernel::kKernargByteLength);
  static_assert(kernel::kPrivateSegmentByteLength == 0);
  static_assert(kernel::kGroupSegmentByteLength == 0);

  GpuMemory* input = nullptr;
  GpuMemory* output = nullptr;
  GpuMemory* arguments = nullptr;
  GpuMemory* completion = nullptr;
  GpuMemory* code = nullptr;
  GpuMemory* indirect_buffer = nullptr;
  ASSERT_NO_FATAL_FAILURE(CreateMemory(AMDF_MEMORY_ACCESS_READ,
                                       kWordCount * sizeof(uint32_t), &input));
  ASSERT_NO_FATAL_FAILURE(
      CreateMemory(AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE,
                   kWordCount * sizeof(uint32_t), &output));
  ASSERT_NO_FATAL_FAILURE(
      CreateMemory(AMDF_MEMORY_ACCESS_READ, kPageByteLength, &arguments));
  ASSERT_NO_FATAL_FAILURE(
      CreateMemory(AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE,
                   kPageByteLength, &completion));
  ASSERT_NO_FATAL_FAILURE(
      CreateMemory(AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_EXECUTE,
                   kPageByteLength, &indirect_buffer));
  ASSERT_EQ(input->device_address % alignof(uint32_t), 0u);
  ASSERT_EQ(output->device_address % alignof(uint32_t), 0u);
  ASSERT_EQ(arguments->device_address % kernel::kKernargAlignment, 0u);
  ASSERT_EQ(completion->device_address % alignof(uint32_t), 0u);
  ASSERT_EQ(indirect_buffer->device_address % kPageByteLength, 0u);
  ASSERT_LE(indirect_buffer->device_address,
            (UINT64_C(1) << 48) - kPageByteLength);
  Pm4ComputeProgram program = {
      0,
      kernel::kComputePgmRsrc1,
      kernel::kComputePgmRsrc2,
      kernel::kComputePgmRsrc3,
      kernel::kGroupSegmentByteLength,
      {kernel::kWorkgroupSize, 1, 1},
  };
  ASSERT_NO_FATAL_FAILURE(PrepareProgram(kernel::kExecutable,
                                         kernel::kEntryByteOffset, &program,
                                         "pm4_command_buffer", &code));

  std::array<uint32_t, kPageWordCount> expected_code = {};
  std::memcpy(expected_code.data(), kernel::kExecutable.words,
              kernel::kExecutable.byte_length);
  std::array<uint32_t, kPageWordCount> expected_indirect = {};
  Pm4CommandWriter indirect(expected_indirect.data());
  indirect.BindCompute(program, arguments->device_address);
  indirect.DispatchWave32(kGridSize, 1, 1);
  // The complete nine-DWORD NOP pads the 31 active words to 40. Retain the
  // entire initialized page, independently of the active command count.
  indirect.PadToEightWords();
  ASSERT_EQ(indirect.word_count(), kIndirectWordCount);
  std::memcpy(indirect_buffer->host.pointer, expected_indirect.data(),
              sizeof(expected_indirect));

  std::array<uint32_t, kPageWordCount> expected_control;
  for (uint32_t word = 0; word < kPageWordCount; ++word) {
    expected_control[word] = 0x68d329b7u ^ word;
  }
  expected_control[kCompletionWord] = 0;
  // Only the GPU changes the marker after this one initialization.
  std::memcpy(completion->host.pointer, expected_control.data(),
              sizeof(expected_control));

  GpuUserQueue* queue = nullptr;
  ASSERT_NO_FATAL_FAILURE(CreateQueue(&queue));
  const size_t ring_word_count =
      queue->host.ring_byte_length / sizeof(uint32_t);
  // Both complete programs stay resident without wrapping and leave the
  // mandatory free DWORD. Unpublished ring bytes are initialized as well.
  ASSERT_GT(ring_word_count, kRingWordsPerEpoch * kCounts.size());
  std::vector<uint32_t> expected_ring(ring_word_count, 0);
  std::vector<uint32_t> observed_ring(ring_word_count);
  std::array<uint64_t, kCounts.size()> frontiers;
  std::array<size_t, kCounts.size()> call_word_offsets;
  Pm4CommandWriter commands(expected_ring.data());
  for (uint32_t epoch = 0; epoch < kCounts.size(); ++epoch) {
    commands.SystemBarrier();
    call_word_offsets[epoch] = commands.word_count();
    commands.CallIndirectBuffer(indirect_buffer->device_address,
                                static_cast<uint32_t>(indirect.word_count()));
    // Return resumes ring parsing; this explicit barrier separately joins
    // the shader and publishes its stores before the confirmed marker.
    commands.SystemBarrier();
    commands.WriteData32(
        completion->device_address + kCompletionWord * sizeof(uint32_t),
        epoch + 1);
    commands.PadToEightWords();
    frontiers[epoch] = commands.word_count();
    ASSERT_EQ(frontiers[epoch], (epoch + 1) * kRingWordsPerEpoch);
  }
  std::memcpy(reinterpret_cast<void*>(queue->host.ring_address),
              expected_ring.data(), queue->host.ring_byte_length);

  RecordProperty("pm4_command_buffer_ib_packet_header",
                 std::to_string(expected_ring[call_word_offsets[0]]));
  RecordProperty("pm4_command_buffer_ib_packet_control",
                 std::to_string(expected_ring[call_word_offsets[0] + 3]));
  RecordProperty("pm4_command_buffer_ib_word_count",
                 std::to_string(indirect.word_count()));
  RecordProperty("pm4_command_buffer_ib_byte_length",
                 std::to_string(indirect.word_count() * sizeof(uint32_t)));
  RecordProperty("pm4_command_buffer_grid_size", kGridSize);
  RecordProperty("pm4_command_buffer_workgroup_size", kernel::kWorkgroupSize);
  RecordProperty("pm4_command_buffer_payload_word_offset", kPayloadOffset);
  RecordProperty("pm4_command_buffer_payload_observed_byte_length",
                 kWordCount * sizeof(uint32_t));
  RecordProperty("pm4_command_buffer_kernarg_byte_length",
                 kernel::kKernargByteLength);
  RecordProperty("pm4_command_buffer_argument_observed_byte_length",
                 kPageByteLength);
  RecordProperty("pm4_command_buffer_completion_byte_offset",
                 kCompletionWord * sizeof(uint32_t));
  RecordProperty("pm4_command_buffer_completion_observed_byte_length",
                 kPageByteLength);
  RecordProperty("pm4_command_buffer_code_observed_byte_length",
                 kPageByteLength);
  RecordProperty("pm4_command_buffer_ib_observed_byte_length", kPageByteLength);
  RecordProperty("pm4_command_buffer_observed_owner_byte_length",
                 2 * kWordCount * sizeof(uint32_t) + 4 * kPageByteLength);
  RecordProperty("pm4_command_buffer_ring_capacity_dwords",
                 std::to_string(ring_word_count));
  RecordProperty("pm4_command_buffer_observed_ring_byte_length",
                 std::to_string(queue->host.ring_byte_length));

  std::array<uint32_t, kWordCount> expected_input;
  std::array<uint32_t, kWordCount> expected_output;
  std::array<uint32_t, kWordCount> observed_input;
  std::array<uint32_t, kWordCount> observed_output;
  std::array<uint8_t, kPageByteLength> expected_arguments;
  std::array<uint8_t, kPageByteLength> observed_arguments;
  std::array<uint32_t, kPageWordCount> observed_control;
  std::array<uint32_t, kPageWordCount> observed_code;
  std::array<uint32_t, kPageWordCount> observed_indirect;
  for (uint32_t epoch = 0; epoch < kCounts.size(); ++epoch) {
    SCOPED_TRACE(epoch);
    expected_input.fill(0x759bf13du ^ (epoch * 0x01010101u));
    expected_output.fill(0x4e90b725u ^ (epoch * 0x01010101u));
    observed_output = expected_output;
    for (uint32_t i = 0; i < kGridSize; ++i) {
      const uint32_t value =
          0xfffffff0u + i * 0x01030507u + epoch * 0x11111111u;
      expected_input[kPayloadOffset + i] = value;
      if (i < kCounts[epoch]) {
        const uint32_t result =
            static_cast<uint32_t>(uint64_t{value} * 3 + kAddends[epoch]);
        expected_output[kPayloadOffset + i] = result;
        observed_output[kPayloadOffset + i] = ~result;
      }
    }
    std::memcpy(input->host.pointer, expected_input.data(),
                sizeof(expected_input));
    std::memcpy(output->host.pointer, observed_output.data(),
                sizeof(observed_output));
    const kernels::transform::Arguments payload = {
        input->device_address + kPayloadOffset * sizeof(uint32_t),
        output->device_address + kPayloadOffset * sizeof(uint32_t),
        kCounts[epoch],
        kAddends[epoch],
    };
    expected_arguments.fill(0);
    // The host structure has alignment padding. Copy only the 24 semantic
    // bytes into fully initialized backing, retaining it through final use.
    std::memcpy(expected_arguments.data(), &payload,
                kernel::kKernargByteLength);
    std::memcpy(arguments->host.pointer, expected_arguments.data(),
                sizeof(expected_arguments));

    queue->PublishStream(frontiers[epoch]);
    GpuWaitEqual<uint32_t>(
        reinterpret_cast<uintptr_t>(completion->host.pointer) +
            kCompletionWord * sizeof(uint32_t),
        epoch + 1);
    // Capture every initialized owner and the complete ring before any
    // diagnostic or consumption wait can add another observation boundary.
    std::memcpy(observed_output.data(), output->host.pointer,
                sizeof(observed_output));
    std::memcpy(observed_input.data(), input->host.pointer,
                sizeof(observed_input));
    std::memcpy(observed_arguments.data(), arguments->host.pointer,
                sizeof(observed_arguments));
    std::memcpy(observed_control.data(), completion->host.pointer,
                sizeof(observed_control));
    std::memcpy(observed_code.data(), code->host.pointer,
                sizeof(observed_code));
    std::memcpy(observed_indirect.data(), indirect_buffer->host.pointer,
                sizeof(observed_indirect));
    std::memcpy(observed_ring.data(),
                reinterpret_cast<const void*>(queue->host.ring_address),
                queue->host.ring_byte_length);

    expected_control[kCompletionWord] = epoch + 1;
    for (uint32_t word = 0; word < kWordCount; ++word) {
      EXPECT_EQ(observed_output[word], expected_output[word])
          << "output word=" << word;
      EXPECT_EQ(observed_input[word], expected_input[word])
          << "input word=" << word;
    }
    for (uint32_t byte = 0; byte < kPageByteLength; ++byte) {
      EXPECT_EQ(observed_arguments[byte], expected_arguments[byte])
          << "argument byte=" << byte;
    }
    for (uint32_t word = 0; word < kPageWordCount; ++word) {
      EXPECT_EQ(observed_control[word], expected_control[word])
          << "control word=" << word;
      EXPECT_EQ(observed_code[word], expected_code[word])
          << "code word=" << word;
      EXPECT_EQ(observed_indirect[word], expected_indirect[word])
          << "indirect-buffer word=" << word;
    }
    for (size_t word = 0; word < ring_word_count; ++word) {
      EXPECT_EQ(observed_ring[word], expected_ring[word])
          << "ring word=" << word;
    }
    // Every nonfatal mismatch still retires the published ring range. No
    // payload or argument rewrite follows a failed observation or retirement.
    EXPECT_NO_FATAL_FAILURE(queue->WaitConsumed(api_, frontiers[epoch]));
    if (HasFailure()) {
      return;
    }
    const std::string prefix =
        "pm4_command_buffer_epoch_" + std::to_string(epoch);
    RecordProperty(prefix + "_count", kCounts[epoch]);
    RecordProperty(prefix + "_addend", std::to_string(kAddends[epoch]));
    RecordProperty(prefix + "_completion", observed_control[kCompletionWord]);
    RecordProperty(prefix + "_call_word_offset",
                   std::to_string(call_word_offsets[epoch]));
    RecordProperty(prefix + "_producer_frontier",
                   std::to_string(frontiers[epoch]));
  }
  RecordProperty("pm4_command_buffer_completed_epochs", kCounts.size());
  RecordProperty("pm4_command_buffer_ring_command_word_count",
                 std::to_string(commands.word_count()));
  RecordProperty("pm4_command_buffer_final_producer_index",
                 std::to_string(frontiers.back()));
}

TEST_F(Pm4DispatchTest, RebuildsIndirectBufferAfterCompletion) {
  constexpr uint32_t kWordCount = 4096;
  constexpr uint32_t kCandidateWordCount = 1024;
  constexpr uint32_t kArgumentByteStride = 128;
  constexpr uint32_t kPageByteLength = 4096;
  constexpr uint32_t kPageWordCount = kPageByteLength / sizeof(uint32_t);
  constexpr uint32_t kIndirectWordCount = 40;
  constexpr uint32_t kKernargLowWord = 24;
  constexpr uint32_t kDispatchXWord = 27;
  constexpr uint32_t kRingWordsPerEpoch = 32;
  constexpr uint32_t kCompletionWord = 0;
  constexpr std::array<uint32_t, 2> kGridSizes = {1024, 576};
  constexpr std::array<uint32_t, 2> kPayloadOffsets = {64, 2112};
  constexpr std::array<uint32_t, 2> kAddends = {7, 0x80000023u};
  static_assert(offsetof(kernels::transform::Arguments, addend) +
                    sizeof(uint32_t) ==
                kernel::kKernargByteLength);
  static_assert(kernel::kPrivateSegmentByteLength == 0);
  static_assert(kernel::kGroupSegmentByteLength == 0);
  static_assert(kPayloadOffsets[0] + kCandidateWordCount <= kPayloadOffsets[1]);
  static_assert(kPayloadOffsets[1] + kCandidateWordCount <= kWordCount);
  static_assert(kArgumentByteStride + kernel::kKernargByteLength <=
                kPageByteLength);

  GpuMemory* input = nullptr;
  GpuMemory* output = nullptr;
  GpuMemory* arguments = nullptr;
  GpuMemory* completion = nullptr;
  GpuMemory* code = nullptr;
  GpuMemory* indirect_buffer = nullptr;
  ASSERT_NO_FATAL_FAILURE(CreateMemory(AMDF_MEMORY_ACCESS_READ,
                                       kWordCount * sizeof(uint32_t), &input));
  ASSERT_NO_FATAL_FAILURE(
      CreateMemory(AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE,
                   kWordCount * sizeof(uint32_t), &output));
  ASSERT_NO_FATAL_FAILURE(
      CreateMemory(AMDF_MEMORY_ACCESS_READ, kPageByteLength, &arguments));
  ASSERT_NO_FATAL_FAILURE(
      CreateMemory(AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE,
                   kPageByteLength, &completion));
  ASSERT_NO_FATAL_FAILURE(
      CreateMemory(AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_EXECUTE,
                   kPageByteLength, &indirect_buffer));
  ASSERT_EQ(input->device_address % alignof(uint32_t), 0u);
  ASSERT_EQ(output->device_address % alignof(uint32_t), 0u);
  // Both argument addresses share their high DWORD without a page crossing.
  ASSERT_EQ(arguments->device_address % kPageByteLength, 0u);
  ASSERT_EQ(completion->device_address % alignof(uint32_t), 0u);
  ASSERT_EQ(indirect_buffer->device_address % kPageByteLength, 0u);
  ASSERT_LE(indirect_buffer->device_address,
            (UINT64_C(1) << 48) - kPageByteLength);
  Pm4ComputeProgram program = {
      0,
      kernel::kComputePgmRsrc1,
      kernel::kComputePgmRsrc2,
      kernel::kComputePgmRsrc3,
      kernel::kGroupSegmentByteLength,
      {kernel::kWorkgroupSize, 1, 1},
  };
  ASSERT_NO_FATAL_FAILURE(PrepareProgram(kernel::kExecutable,
                                         kernel::kEntryByteOffset, &program,
                                         "pm4_command_rebuild", &code));

  std::array<uint32_t, kPageWordCount> expected_code = {};
  std::memcpy(expected_code.data(), kernel::kExecutable.words,
              kernel::kExecutable.byte_length);
  std::array<uint8_t, kPageByteLength> expected_arguments = {};
  std::array<std::array<uint32_t, kPageWordCount>, kGridSizes.size()>
      indirect_images = {};
  for (uint32_t epoch = 0; epoch < kGridSizes.size(); ++epoch) {
    const kernels::transform::Arguments payload = {
        input->device_address + kPayloadOffsets[epoch] * sizeof(uint32_t),
        output->device_address + kPayloadOffsets[epoch] * sizeof(uint32_t),
        kCandidateWordCount,
        kAddends[epoch],
    };
    // Only semantic bytes are copied; all structure padding and page guards
    // are initialized independently and remain unchanged across both uses.
    std::memcpy(expected_arguments.data() + epoch * kArgumentByteStride,
                &payload, kernel::kKernargByteLength);
    Pm4CommandWriter indirect(indirect_images[epoch].data());
    indirect.BindCompute(
        program, arguments->device_address + epoch * kArgumentByteStride);
    indirect.DispatchWave32(kGridSizes[epoch], 1, 1);
    indirect.PadToEightWords();
    ASSERT_EQ(indirect.word_count(), kIndirectWordCount);
    ASSERT_EQ(kGridSizes[epoch] % kernel::kWorkgroupSize, 0u);
    ASSERT_LE(kGridSizes[epoch], kCandidateWordCount);
  }
  for (uint32_t word = 0; word < kPageWordCount; ++word) {
    if (word == kKernargLowWord || word == kDispatchXWord) {
      ASSERT_NE(indirect_images[0][word], indirect_images[1][word]);
    } else {
      ASSERT_EQ(indirect_images[0][word], indirect_images[1][word]);
    }
  }
  std::memcpy(arguments->host.pointer, expected_arguments.data(),
              sizeof(expected_arguments));

  std::array<uint32_t, kPageWordCount> expected_control;
  for (uint32_t word = 0; word < kPageWordCount; ++word) {
    expected_control[word] = 0x68d329b7u ^ word;
  }
  expected_control[kCompletionWord] = 0;
  // The host initializes the marker once; only the GPU advances it.
  std::memcpy(completion->host.pointer, expected_control.data(),
              sizeof(expected_control));

  GpuUserQueue* queue = nullptr;
  ASSERT_NO_FATAL_FAILURE(CreateQueue(&queue));
  const size_t ring_word_count =
      queue->host.ring_byte_length / sizeof(uint32_t);
  ASSERT_GT(ring_word_count, kRingWordsPerEpoch * kGridSizes.size());
  std::vector<uint32_t> expected_ring(ring_word_count, 0);
  std::vector<uint32_t> observed_ring(ring_word_count);
  std::array<uint64_t, kGridSizes.size()> frontiers;
  std::array<size_t, kGridSizes.size()> call_word_offsets;
  Pm4CommandWriter commands(expected_ring.data());
  for (uint32_t epoch = 0; epoch < kGridSizes.size(); ++epoch) {
    commands.SystemBarrier();
    call_word_offsets[epoch] = commands.word_count();
    commands.CallIndirectBuffer(indirect_buffer->device_address,
                                kIndirectWordCount);
    // This resumed-ring join is separate from command return and consumption.
    commands.SystemBarrier();
    commands.WriteData32(
        completion->device_address + kCompletionWord * sizeof(uint32_t),
        epoch + 1);
    commands.PadToEightWords();
    frontiers[epoch] = commands.word_count();
    ASSERT_EQ(frontiers[epoch], (epoch + 1) * kRingWordsPerEpoch);
  }
  std::memcpy(reinterpret_cast<void*>(queue->host.ring_address),
              expected_ring.data(), queue->host.ring_byte_length);

  RecordProperty("pm4_command_rebuild_ib_packet_header",
                 std::to_string(expected_ring[call_word_offsets[0]]));
  RecordProperty("pm4_command_rebuild_ib_packet_control",
                 std::to_string(expected_ring[call_word_offsets[0] + 3]));
  RecordProperty("pm4_command_rebuild_ib_word_count", kIndirectWordCount);
  RecordProperty("pm4_command_rebuild_ib_byte_length",
                 kIndirectWordCount * sizeof(uint32_t));
  RecordProperty("pm4_command_rebuild_changed_ib_word_0", kKernargLowWord);
  RecordProperty("pm4_command_rebuild_changed_ib_word_1", kDispatchXWord);
  RecordProperty("pm4_command_rebuild_workgroup_size", kernel::kWorkgroupSize);
  RecordProperty("pm4_command_rebuild_argument_record_count",
                 kGridSizes.size());
  RecordProperty("pm4_command_rebuild_argument_byte_stride",
                 kArgumentByteStride);
  RecordProperty("pm4_command_rebuild_argument_count", kCandidateWordCount);
  RecordProperty("pm4_command_rebuild_payload_candidate_word_count",
                 kCandidateWordCount);
  RecordProperty("pm4_command_rebuild_payload_observed_byte_length",
                 kWordCount * sizeof(uint32_t));
  RecordProperty("pm4_command_rebuild_kernarg_byte_length",
                 kernel::kKernargByteLength);
  RecordProperty("pm4_command_rebuild_argument_observed_byte_length",
                 kPageByteLength);
  RecordProperty("pm4_command_rebuild_completion_byte_offset",
                 kCompletionWord * sizeof(uint32_t));
  RecordProperty("pm4_command_rebuild_completion_observed_byte_length",
                 kPageByteLength);
  RecordProperty("pm4_command_rebuild_code_observed_byte_length",
                 kPageByteLength);
  RecordProperty("pm4_command_rebuild_ib_observed_byte_length",
                 kPageByteLength);
  RecordProperty("pm4_command_rebuild_observed_owner_byte_length",
                 2 * kWordCount * sizeof(uint32_t) + 4 * kPageByteLength);
  RecordProperty("pm4_command_rebuild_ring_capacity_dwords",
                 std::to_string(ring_word_count));
  RecordProperty("pm4_command_rebuild_observed_ring_byte_length",
                 std::to_string(queue->host.ring_byte_length));

  std::array<uint32_t, kWordCount> expected_input;
  std::array<uint32_t, kWordCount> expected_output;
  std::array<uint32_t, kWordCount> observed_input;
  std::array<uint32_t, kWordCount> observed_output;
  std::array<uint8_t, kPageByteLength> observed_arguments;
  std::array<uint32_t, kPageWordCount> observed_control;
  std::array<uint32_t, kPageWordCount> observed_code;
  std::array<uint32_t, kPageWordCount> observed_indirect;
  for (uint32_t epoch = 0; epoch < kGridSizes.size(); ++epoch) {
    SCOPED_TRACE(epoch);
    for (uint32_t word = 0; word < kWordCount; ++word) {
      expected_input[word] = 0x759bf13du ^ word ^ (epoch * 0x01010101u);
      expected_output[word] = 0x4e90b725u ^ word ^ (epoch * 0x01010101u);
    }
    observed_output = expected_output;
    for (uint32_t region = 0; region < kPayloadOffsets.size(); ++region) {
      for (uint32_t i = 0; i < kCandidateWordCount; ++i) {
        const uint32_t word = kPayloadOffsets[region] + i;
        const uint32_t value = 0xfffffff0u + i * 0x01030507u +
                               region * 0x22222223u + epoch * 0x11111111u;
        expected_input[word] = value;
        const uint32_t result =
            static_cast<uint32_t>(uint64_t{value} * 3 + kAddends[region]);
        // Both possible bindings have a full backed region poisoned against
        // every valid stale write, including the previous larger grid.
        observed_output[word] = ~result;
        expected_output[word] =
            region == epoch && i < kGridSizes[epoch] ? result : ~result;
      }
    }
    // Only a successfully observed and consumed prior call reaches this
    // rewrite. The whole IB page remains owned through queue removal.
    std::memcpy(indirect_buffer->host.pointer, indirect_images[epoch].data(),
                sizeof(indirect_images[epoch]));
    std::memcpy(input->host.pointer, expected_input.data(),
                sizeof(expected_input));
    std::memcpy(output->host.pointer, observed_output.data(),
                sizeof(observed_output));

    queue->PublishStream(frontiers[epoch]);
    GpuWaitEqual<uint32_t>(
        reinterpret_cast<uintptr_t>(completion->host.pointer) +
            kCompletionWord * sizeof(uint32_t),
        epoch + 1);
    // Observe all initialized storage before diagnostics or retirement can
    // create another completion/visibility boundary.
    std::memcpy(observed_output.data(), output->host.pointer,
                sizeof(observed_output));
    std::memcpy(observed_input.data(), input->host.pointer,
                sizeof(observed_input));
    std::memcpy(observed_arguments.data(), arguments->host.pointer,
                sizeof(observed_arguments));
    std::memcpy(observed_control.data(), completion->host.pointer,
                sizeof(observed_control));
    std::memcpy(observed_code.data(), code->host.pointer,
                sizeof(observed_code));
    std::memcpy(observed_indirect.data(), indirect_buffer->host.pointer,
                sizeof(observed_indirect));
    std::memcpy(observed_ring.data(),
                reinterpret_cast<const void*>(queue->host.ring_address),
                queue->host.ring_byte_length);

    expected_control[kCompletionWord] = epoch + 1;
    for (uint32_t word = 0; word < kWordCount; ++word) {
      EXPECT_EQ(observed_output[word], expected_output[word])
          << "output word=" << word;
      EXPECT_EQ(observed_input[word], expected_input[word])
          << "input word=" << word;
    }
    for (uint32_t byte = 0; byte < kPageByteLength; ++byte) {
      EXPECT_EQ(observed_arguments[byte], expected_arguments[byte])
          << "argument byte=" << byte;
    }
    for (uint32_t word = 0; word < kPageWordCount; ++word) {
      EXPECT_EQ(observed_control[word], expected_control[word])
          << "control word=" << word;
      EXPECT_EQ(observed_code[word], expected_code[word])
          << "code word=" << word;
      EXPECT_EQ(observed_indirect[word], indirect_images[epoch][word])
          << "indirect-buffer word=" << word;
    }
    for (size_t word = 0; word < ring_word_count; ++word) {
      EXPECT_EQ(observed_ring[word], expected_ring[word])
          << "ring word=" << word;
    }
    EXPECT_NO_FATAL_FAILURE(queue->WaitConsumed(api_, frontiers[epoch]));
    if (HasFailure()) {
      return;
    }
    const std::string prefix =
        "pm4_command_rebuild_epoch_" + std::to_string(epoch);
    RecordProperty(prefix + "_grid_size", kGridSizes[epoch]);
    RecordProperty(prefix + "_argument_byte_offset",
                   epoch * kArgumentByteStride);
    RecordProperty(prefix + "_payload_word_offset", kPayloadOffsets[epoch]);
    RecordProperty(prefix + "_addend", std::to_string(kAddends[epoch]));
    RecordProperty(prefix + "_completion", observed_control[kCompletionWord]);
    RecordProperty(prefix + "_call_word_offset",
                   std::to_string(call_word_offsets[epoch]));
    RecordProperty(prefix + "_producer_frontier",
                   std::to_string(frontiers[epoch]));
  }
  RecordProperty("pm4_command_rebuild_completed_epochs", kGridSizes.size());
  RecordProperty("pm4_command_rebuild_ib_upload_count", kGridSizes.size());
  RecordProperty("pm4_command_rebuild_ib_rebuild_count", kGridSizes.size() - 1);
  RecordProperty("pm4_command_rebuild_ring_command_word_count",
                 std::to_string(commands.word_count()));
  RecordProperty("pm4_command_rebuild_final_producer_index",
                 std::to_string(frontiers.back()));
}

}  // namespace
