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
#include "libamdf/cts/gpu/kernels/transform_kernels.h"
#include "libamdf/cts/gpu/pm4/dispatch_fixture.h"
#include "libamdf/cts/gpu/pm4/encoding/commands.h"

namespace {

TEST_F(Pm4DispatchTest, ExecutesImmutableIndirectBufferAcrossEpochs) {
  const auto* kernel_product =
      kernels::transform::kKernels.Find(gpu_endpoint_info_);
  ASSERT_NE(kernel_product, nullptr)
      << "missing compiled transform kernel for endpoint";
  const auto& kernel = *kernel_product;
  RecordProperty("transform_kernel_target", kernel.target);

  constexpr uint32_t kGridSize = 1024;
  constexpr uint32_t kWordCount = 2048;
  constexpr uint32_t kPayloadOffset = 16;
  constexpr uint32_t kPageByteLength = 4096;
  constexpr uint32_t kPageWordCount = kPageByteLength / sizeof(uint32_t);
  constexpr uint32_t kIndirectWordCount = 64;
  constexpr uint32_t kCommandWordsPerEpoch = 8;
  constexpr uint32_t kCompletionWord = 0;
  constexpr uint32_t kEpochByteOffset = 64;
  constexpr std::array<uint32_t, 2> kCounts = {1003, 997};
  constexpr std::array<uint32_t, 2> kAddends = {7, 0x80000023u};

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
  ASSERT_EQ(arguments->device_address % kernel.arguments.alignment, 0u);
  ASSERT_EQ(completion->device_address % alignof(uint32_t), 0u);
  ASSERT_EQ(indirect_buffer->device_address % kPageByteLength, 0u);
  ASSERT_LE(indirect_buffer->device_address,
            (UINT64_C(1) << 48) - kPageByteLength);
  Pm4ComputeProgram program = {
      0,
      kernel.program.resource1,
      kernel.program.resource2,
      kernel.program.resource3,
      kernel.group_segment_byte_length,
      {kernel.workgroup_size(), 1, 1},
  };
  ASSERT_NO_FATAL_FAILURE(PrepareProgram(kernel.executable,
                                         kernel.entry_byte_offset, &program,
                                         "pm4_command_buffer", &code));

  std::array<uint32_t, kPageWordCount> expected_code = {};
  std::memcpy(expected_code.data(), kernel.executable.words,
              kernel.executable.byte_length);
  std::array<uint32_t, kPageWordCount> expected_indirect = {};
  Pm4CommandWriter indirect(expected_indirect.data(), *pm4_profile_);
  indirect.SystemBarrier();
  indirect.BindCompute(program, arguments->device_address);
  indirect.DispatchWave32(kGridSize, 1, 1);
  indirect.SystemBarrier();
  // The immutable IB reads its completion epoch separately from the shader's
  // kernarg ABI. Completion is part of the IB on both publication transports.
  ASSERT_LE(kernel.arguments.byte_length, kEpochByteOffset);
  indirect.CopyData32(arguments->device_address + kEpochByteOffset,
                      completion->device_address);
  // Retain the entire initialized page, including the complete final NOP.
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

  GpuCommandQueue* queue = nullptr;
  ASSERT_NO_FATAL_FAILURE(CreateQueue(&queue));
  const bool primary_ring =
      queue->publication_mode() == AMDF_QUEUE_PUBLICATION_MODE_USER;
  const size_t command_capacity = primary_ring ? queue->words().size() : 0;
  // Both complete programs stay resident without wrapping and leave the
  // mandatory free DWORD. Unpublished command storage bytes are initialized as
  // well.
  if (primary_ring) {
    ASSERT_GT(command_capacity, kCommandWordsPerEpoch * kCounts.size());
  }
  std::vector<uint32_t> expected_commands(command_capacity, 0);
  std::vector<uint32_t> observed_commands(command_capacity);
  std::array<uint64_t, kCounts.size()> frontiers = {};
  std::array<size_t, kCounts.size()> entry_word_offsets = {};
  Pm4CommandWriter commands(expected_commands.data(), *pm4_profile_);
  for (uint32_t epoch = 0; epoch < kCounts.size(); ++epoch) {
    if (primary_ring) {
      entry_word_offsets[epoch] = commands.word_count();
      commands.CallIndirectBuffer(indirect_buffer->device_address,
                                  kIndirectWordCount);
      commands.PadToEightWords();
      frontiers[epoch] = commands.word_count();
      ASSERT_EQ(frontiers[epoch], (epoch + 1) * kCommandWordsPerEpoch);
    }
  }
  if (primary_ring) {
    std::memcpy(queue->words().data(), expected_commands.data(),
                queue->words().size_bytes());
  }

  RecordProperty("pm4_command_buffer_entry",
                 primary_ring ? "primary_ring_call" : "kernel_submission");
  if (primary_ring) {
    RecordProperty("pm4_command_buffer_ib_packet_header",
                   std::to_string(expected_commands[entry_word_offsets[0]]));
    RecordProperty(
        "pm4_command_buffer_ib_packet_control",
        std::to_string(expected_commands[entry_word_offsets[0] + 3]));
  }
  RecordProperty("pm4_command_buffer_ib_word_count",
                 std::to_string(indirect.word_count()));
  RecordProperty("pm4_command_buffer_ib_byte_length",
                 std::to_string(indirect.word_count() * sizeof(uint32_t)));
  RecordProperty("pm4_command_buffer_grid_size", kGridSize);
  RecordProperty("pm4_command_buffer_workgroup_size", kernel.workgroup_size());
  RecordProperty("pm4_command_buffer_payload_word_offset", kPayloadOffset);
  RecordProperty("pm4_command_buffer_payload_observed_byte_length",
                 kWordCount * sizeof(uint32_t));
  RecordProperty("pm4_command_buffer_kernarg_byte_length",
                 kernel.arguments.byte_length);
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
  RecordProperty("pm4_command_buffer_command_capacity_dwords",
                 std::to_string(command_capacity));
  RecordProperty("pm4_command_buffer_observed_commands_byte_length",
                 std::to_string(command_capacity * sizeof(uint32_t)));

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
                kernel.arguments.byte_length);
    const uint32_t completion_epoch = epoch + 1;
    std::memcpy(expected_arguments.data() + kEpochByteOffset, &completion_epoch,
                sizeof(completion_epoch));
    std::memcpy(arguments->host.pointer, expected_arguments.data(),
                sizeof(expected_arguments));

    if (primary_ring) {
      ASSERT_NO_FATAL_FAILURE(queue->Publish(api_, gpu_api_, frontiers[epoch]));
    } else {
      // KERNEL consumes the IB itself. A ring-style call inside that buffer
      // would incorrectly request compute IB2 nesting.
      const amdf_gpu_kernel_command_t command = {
          .memory = indirect_buffer->memory,
          .byte_offset = 0,
          .byte_length = kIndirectWordCount * sizeof(uint32_t),
      };
      ASSERT_NO_FATAL_FAILURE(queue->Submit(gpu_api_, command));
    }
    GpuWaitEqual<uint32_t>(
        reinterpret_cast<uintptr_t>(completion->host.pointer) +
            kCompletionWord * sizeof(uint32_t),
        epoch + 1);
    // Capture every initialized owner and the complete command storage before
    // any diagnostic or retirement wait can add another observation boundary.
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
    if (primary_ring) {
      std::memcpy(observed_commands.data(), queue->words().data(),
                  queue->words().size_bytes());
    }

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
    for (size_t word = 0; word < command_capacity; ++word) {
      EXPECT_EQ(observed_commands[word], expected_commands[word])
          << "command storage word=" << word;
    }
    // Every nonfatal mismatch still retires the published command storage
    // range. No payload or argument rewrite follows a failed observation or
    // retirement.
    EXPECT_NO_FATAL_FAILURE(queue->WaitRetired(api_));
    if (HasFailure()) {
      return;
    }
    const std::string prefix =
        "pm4_command_buffer_epoch_" + std::to_string(epoch);
    RecordProperty(prefix + "_count", kCounts[epoch]);
    RecordProperty(prefix + "_addend", std::to_string(kAddends[epoch]));
    RecordProperty(prefix + "_completion", observed_control[kCompletionWord]);
    if (primary_ring) {
      RecordProperty(prefix + "_entry_word_offset",
                     std::to_string(entry_word_offsets[epoch]));
      RecordProperty(prefix + "_producer_frontier",
                     std::to_string(frontiers[epoch]));
    }
  }
  RecordProperty("pm4_command_buffer_completed_epochs", kCounts.size());
  RecordProperty("pm4_command_buffer_command_word_count",
                 std::to_string(commands.word_count()));
  RecordProperty("pm4_command_buffer_final_published_word_count",
                 std::to_string(frontiers.back()));
}

TEST_F(Pm4DispatchTest, RebuildsIndirectBufferAfterCompletion) {
  const auto* kernel_product =
      kernels::transform::kKernels.Find(gpu_endpoint_info_);
  ASSERT_NE(kernel_product, nullptr)
      << "missing compiled transform kernel for endpoint";
  const auto& kernel = *kernel_product;
  RecordProperty("transform_kernel_target", kernel.target);

  constexpr uint32_t kWordCount = 4096;
  constexpr uint32_t kCandidateWordCount = 1024;
  constexpr uint32_t kArgumentByteStride = 128;
  constexpr uint32_t kPageByteLength = 4096;
  constexpr uint32_t kPageWordCount = kPageByteLength / sizeof(uint32_t);
  constexpr uint32_t kIndirectWordCount = 64;
  constexpr uint32_t kKernargLowWord = 34;
  constexpr uint32_t kDispatchXWord = 37;
  constexpr uint32_t kCompletionValueWord = 55;
  constexpr uint32_t kCommandWordsPerEpoch = 8;
  constexpr uint32_t kCompletionWord = 0;
  constexpr std::array<uint32_t, 2> kGridSizes = {1024, 576};
  constexpr std::array<uint32_t, 2> kPayloadOffsets = {64, 2112};
  constexpr std::array<uint32_t, 2> kAddends = {7, 0x80000023u};

  static_assert(kPayloadOffsets[0] + kCandidateWordCount <= kPayloadOffsets[1]);
  static_assert(kPayloadOffsets[1] + kCandidateWordCount <= kWordCount);

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
      kernel.program.resource1,
      kernel.program.resource2,
      kernel.program.resource3,
      kernel.group_segment_byte_length,
      {kernel.workgroup_size(), 1, 1},
  };
  ASSERT_NO_FATAL_FAILURE(PrepareProgram(kernel.executable,
                                         kernel.entry_byte_offset, &program,
                                         "pm4_command_rebuild", &code));

  std::array<uint32_t, kPageWordCount> expected_code = {};
  std::memcpy(expected_code.data(), kernel.executable.words,
              kernel.executable.byte_length);
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
                &payload, kernel.arguments.byte_length);
    Pm4CommandWriter indirect(indirect_images[epoch].data(), *pm4_profile_);
    indirect.SystemBarrier();
    indirect.BindCompute(
        program, arguments->device_address + epoch * kArgumentByteStride);
    indirect.DispatchWave32(kGridSizes[epoch], 1, 1);
    indirect.SystemBarrier();
    indirect.WriteData32(completion->device_address, epoch + 1);
    indirect.PadToEightWords();
    ASSERT_EQ(indirect.word_count(), kIndirectWordCount);
    ASSERT_EQ(kGridSizes[epoch] % kernel.workgroup_size(), 0u);
    ASSERT_LE(kGridSizes[epoch], kCandidateWordCount);
  }
  for (uint32_t word = 0; word < kPageWordCount; ++word) {
    if (word == kKernargLowWord || word == kDispatchXWord ||
        word == kCompletionValueWord) {
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

  GpuCommandQueue* queue = nullptr;
  ASSERT_NO_FATAL_FAILURE(CreateQueue(&queue));
  const bool primary_ring =
      queue->publication_mode() == AMDF_QUEUE_PUBLICATION_MODE_USER;
  const size_t command_capacity = primary_ring ? queue->words().size() : 0;
  if (primary_ring) {
    ASSERT_GT(command_capacity, kCommandWordsPerEpoch * kGridSizes.size());
  }
  std::vector<uint32_t> expected_commands(command_capacity, 0);
  std::vector<uint32_t> observed_commands(command_capacity);
  std::array<uint64_t, kGridSizes.size()> frontiers = {};
  std::array<size_t, kGridSizes.size()> entry_word_offsets = {};
  Pm4CommandWriter commands(expected_commands.data(), *pm4_profile_);
  for (uint32_t epoch = 0; epoch < kGridSizes.size(); ++epoch) {
    if (primary_ring) {
      entry_word_offsets[epoch] = commands.word_count();
      commands.CallIndirectBuffer(indirect_buffer->device_address,
                                  kIndirectWordCount);
      commands.PadToEightWords();
      frontiers[epoch] = commands.word_count();
      ASSERT_EQ(frontiers[epoch], (epoch + 1) * kCommandWordsPerEpoch);
    }
  }
  if (primary_ring) {
    std::memcpy(queue->words().data(), expected_commands.data(),
                queue->words().size_bytes());
  }

  RecordProperty("pm4_command_rebuild_entry",
                 primary_ring ? "primary_ring_call" : "kernel_submission");
  if (primary_ring) {
    RecordProperty("pm4_command_rebuild_ib_packet_header",
                   std::to_string(expected_commands[entry_word_offsets[0]]));
    RecordProperty(
        "pm4_command_rebuild_ib_packet_control",
        std::to_string(expected_commands[entry_word_offsets[0] + 3]));
  }
  RecordProperty("pm4_command_rebuild_ib_word_count", kIndirectWordCount);
  RecordProperty("pm4_command_rebuild_ib_byte_length",
                 kIndirectWordCount * sizeof(uint32_t));
  RecordProperty("pm4_command_rebuild_changed_ib_word_0", kKernargLowWord);
  RecordProperty("pm4_command_rebuild_changed_ib_word_1", kDispatchXWord);
  RecordProperty("pm4_command_rebuild_changed_ib_word_2", kCompletionValueWord);
  RecordProperty("pm4_command_rebuild_workgroup_size", kernel.workgroup_size());
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
                 kernel.arguments.byte_length);
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
  RecordProperty("pm4_command_rebuild_command_capacity_dwords",
                 std::to_string(command_capacity));
  RecordProperty("pm4_command_rebuild_observed_commands_byte_length",
                 std::to_string(command_capacity * sizeof(uint32_t)));

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
    // Only a successfully observed and retired prior call reaches this
    // rewrite. The whole IB page remains owned through queue removal.
    std::memcpy(indirect_buffer->host.pointer, indirect_images[epoch].data(),
                sizeof(indirect_images[epoch]));
    std::memcpy(input->host.pointer, expected_input.data(),
                sizeof(expected_input));
    std::memcpy(output->host.pointer, observed_output.data(),
                sizeof(observed_output));

    if (primary_ring) {
      ASSERT_NO_FATAL_FAILURE(queue->Publish(api_, gpu_api_, frontiers[epoch]));
    } else {
      // KERNEL consumes the IB itself. A ring-style call inside that buffer
      // would incorrectly request compute IB2 nesting.
      const amdf_gpu_kernel_command_t command = {
          .memory = indirect_buffer->memory,
          .byte_offset = 0,
          .byte_length = kIndirectWordCount * sizeof(uint32_t),
      };
      ASSERT_NO_FATAL_FAILURE(queue->Submit(gpu_api_, command));
    }
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
    if (primary_ring) {
      std::memcpy(observed_commands.data(), queue->words().data(),
                  queue->words().size_bytes());
    }

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
    for (size_t word = 0; word < command_capacity; ++word) {
      EXPECT_EQ(observed_commands[word], expected_commands[word])
          << "command storage word=" << word;
    }
    EXPECT_NO_FATAL_FAILURE(queue->WaitRetired(api_));
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
    if (primary_ring) {
      RecordProperty(prefix + "_entry_word_offset",
                     std::to_string(entry_word_offsets[epoch]));
      RecordProperty(prefix + "_producer_frontier",
                     std::to_string(frontiers[epoch]));
    }
  }
  RecordProperty("pm4_command_rebuild_completed_epochs", kGridSizes.size());
  RecordProperty("pm4_command_rebuild_ib_upload_count", kGridSizes.size());
  RecordProperty("pm4_command_rebuild_ib_rebuild_count", kGridSizes.size() - 1);
  RecordProperty("pm4_command_rebuild_command_word_count",
                 std::to_string(commands.word_count()));
  RecordProperty("pm4_command_rebuild_final_published_word_count",
                 std::to_string(frontiers.back()));
}

}  // namespace
