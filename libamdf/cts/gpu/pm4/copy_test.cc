// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <array>
#include <cstring>
#include <string>

#include "libamdf/cts/gpu/pm4/command_fixture.h"
#include "libamdf/cts/gpu/pm4/encoding/commands.h"

namespace {

class Pm4CopyTest : public Pm4CommandTest {};

TEST_F(Pm4CopyTest, CopiesBetweenExactAccessAttachments) {
  GpuMemory* source = nullptr;
  GpuMemory* target = nullptr;
  GpuMemory* completion = nullptr;
  ASSERT_NO_FATAL_FAILURE(CreateMemory(AMDF_MEMORY_ACCESS_READ, 4096, &source));
  ASSERT_NO_FATAL_FAILURE(CreateMemory(
      AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE, 4096, &target));
  ASSERT_NO_FATAL_FAILURE(CreateMemory(
      AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE, 4096, &completion));
  auto* input = static_cast<uint32_t*>(source->host.pointer);
  auto* output = static_cast<uint32_t*>(target->host.pointer);
  *static_cast<uint32_t*>(completion->host.pointer) = 0;
  constexpr size_t kWordCount = 16;
  for (size_t i = 0; i < kWordCount; ++i) {
    input[i] = 0x13570000u + static_cast<uint32_t>(i) * 0x00110101u;
    output[i] = ~input[i];
  }

  GpuCommandQueue* queue = nullptr;
  ASSERT_NO_FATAL_FAILURE(CreateQueue(&queue));
  EXPECT_TRUE(amdf_device_id_is_equal(&queue->device_id(),
                                      &source->access_info.device_id));
  EXPECT_TRUE(amdf_device_id_is_equal(&queue->device_id(),
                                      &target->access_info.device_id));
  ASSERT_GT(queue->words().size_bytes(), 512u);
  Pm4CommandWriter commands(queue->words().data(), *pm4_profile_);
  commands.SystemBarrier();
  for (size_t i = 0; i < kWordCount; ++i) {
    commands.CopyData32(source->device_address + i * sizeof(uint32_t),
                        target->device_address + i * sizeof(uint32_t));
  }
  commands.SystemBarrier();
  commands.WriteData32(completion->device_address, 1);
  commands.PadToEightWords();
  ASSERT_NO_FATAL_FAILURE(
      queue->Publish(api_, gpu_api_, commands.word_count()));
  GpuWaitEqual<uint32_t>(reinterpret_cast<uintptr_t>(completion->host.pointer),
                         1);
  // Capture every observation before diagnostics or retirement can intervene.
  std::array<uint32_t, kWordCount> observed_output;
  std::array<uint32_t, kWordCount> observed_input;
  std::memcpy(observed_output.data(), output, sizeof(observed_output));
  std::memcpy(observed_input.data(), input, sizeof(observed_input));
  for (size_t i = 0; i < kWordCount; ++i) {
    const uint32_t expected =
        0x13570000u + static_cast<uint32_t>(i) * 0x00110101u;
    EXPECT_EQ(observed_output[i], expected) << i;
    EXPECT_EQ(observed_input[i], expected) << "source word " << i;
  }
  // Nonfatal oracle failures still reach normal retirement.
  ASSERT_NO_FATAL_FAILURE(queue->WaitRetired(api_));
}

class Pm4CopyWidthTest : public Pm4CommandTest,
                         public ::testing::WithParamInterface<size_t> {};

TEST_P(Pm4CopyWidthTest, PreservesAllWordsOutsideSelectedTransfers) {
  constexpr size_t kWordCount = 4096 / sizeof(uint32_t);
  const size_t copy_byte_length = GetParam();
  // A 32-bit COPY_DATA at the last source DWORD can read into the next page
  // on gfx1100. Keep that page owned and readable while retaining the exact
  // destination extent and all transfer positions. The 64-bit source still
  // ends at the last selected QWORD.
  const size_t source_word_count =
      copy_byte_length == 4 ? 2 * kWordCount : kWordCount;
  const std::array<size_t, 4> positions =
      copy_byte_length == 4 ? std::array<size_t, 4>{1, 15, 17, kWordCount - 1}
                            : std::array<size_t, 4>{2, 14, 16, kWordCount - 2};
  GpuMemory* source = nullptr;
  GpuMemory* target = nullptr;
  GpuMemory* completion = nullptr;
  ASSERT_NO_FATAL_FAILURE(CreateMemory(
      AMDF_MEMORY_ACCESS_READ, source_word_count * sizeof(uint32_t), &source));
  ASSERT_NO_FATAL_FAILURE(CreateMemory(
      AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE, 4096, &target));
  ASSERT_NO_FATAL_FAILURE(CreateMemory(
      AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE, 4096, &completion));
  auto* input = static_cast<uint32_t*>(source->host.pointer);
  auto* output = static_cast<uint32_t*>(target->host.pointer);
  *static_cast<uint32_t*>(completion->host.pointer) = 0;
  std::array<uint32_t, kWordCount> expected;
  for (size_t i = 0; i < source_word_count; ++i) {
    input[i] = 0x13579bdfu + static_cast<uint32_t>(i) * 0x10203041u;
  }
  for (size_t i = 0; i < kWordCount; ++i) {
    expected[i] = output[i] = ~input[i];
  }

  GpuCommandQueue* queue = nullptr;
  ASSERT_NO_FATAL_FAILURE(CreateQueue(&queue));
  ASSERT_GE(queue->words().size_bytes(), 256u);
  Pm4CommandWriter commands(queue->words().data(), *pm4_profile_);
  commands.SystemBarrier();
  for (size_t position : positions) {
    const uint64_t offset = position * sizeof(uint32_t);
    if (copy_byte_length == 4) {
      commands.CopyData32(source->device_address + offset,
                          target->device_address + offset);
    } else {
      commands.CopyData64(source->device_address + offset,
                          target->device_address + offset);
    }
    for (size_t i = 0; i < copy_byte_length / sizeof(uint32_t); ++i) {
      expected[position + i] = input[position + i];
    }
  }
  commands.SystemBarrier();
  commands.WriteData32(completion->device_address, 1);
  commands.PadToEightWords();
  ASSERT_NO_FATAL_FAILURE(
      queue->Publish(api_, gpu_api_, commands.word_count()));
  GpuWaitEqual<uint32_t>(reinterpret_cast<uintptr_t>(completion->host.pointer),
                         1);
  // Capture every observation before diagnostics or retirement can intervene.
  std::array<uint32_t, kWordCount> observed_output;
  std::array<uint32_t, 2 * kWordCount> observed_input;
  std::memcpy(observed_output.data(), output, sizeof(observed_output));
  std::memcpy(observed_input.data(), input,
              source_word_count * sizeof(uint32_t));
  for (size_t i = 0; i < kWordCount; ++i) {
    EXPECT_EQ(observed_output[i], expected[i]) << i;
  }
  for (size_t i = 0; i < source_word_count; ++i) {
    const uint32_t expected_input =
        0x13579bdfu + static_cast<uint32_t>(i) * 0x10203041u;
    EXPECT_EQ(observed_input[i], expected_input) << "source word " << i;
  }
  // Nonfatal oracle failures still reach normal retirement.
  ASSERT_NO_FATAL_FAILURE(queue->WaitRetired(api_));
}

INSTANTIATE_TEST_SUITE_P(Width, Pm4CopyWidthTest, ::testing::Values(4, 8),
                         [](const ::testing::TestParamInfo<size_t>& info) {
                           return info.param == 4 ? "Dword" : "Qword";
                         });

TEST_F(Pm4CopyTest, ConfirmedWideCopiesFeedTheNextCopy) {
  constexpr size_t kValueCount = 16;
  GpuMemory* source = nullptr;
  GpuMemory* intermediate = nullptr;
  GpuMemory* target = nullptr;
  GpuMemory* completion = nullptr;
  ASSERT_NO_FATAL_FAILURE(CreateMemory(AMDF_MEMORY_ACCESS_READ, 4096, &source));
  ASSERT_NO_FATAL_FAILURE(CreateMemory(
      AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE, 4096, &intermediate));
  ASSERT_NO_FATAL_FAILURE(CreateMemory(
      AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE, 4096, &target));
  ASSERT_NO_FATAL_FAILURE(CreateMemory(
      AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE, 4096, &completion));
  auto* input = static_cast<uint64_t*>(source->host.pointer);
  auto* staging = static_cast<uint64_t*>(intermediate->host.pointer);
  auto* output = static_cast<uint64_t*>(target->host.pointer);
  *static_cast<uint32_t*>(completion->host.pointer) = 0;
  for (size_t i = 0; i < kValueCount; ++i) {
    input[i] = UINT64_C(0x13579bdf2468ace0) + i * UINT64_C(0x0102030405060708);
    staging[i] = output[i] = ~input[i];
  }
  GpuCommandQueue* queue = nullptr;
  ASSERT_NO_FATAL_FAILURE(CreateQueue(&queue));
  ASSERT_GE(queue->words().size_bytes(), 1024u);
  Pm4CommandWriter commands(queue->words().data(), *pm4_profile_);
  commands.SystemBarrier();
  for (size_t i = 0; i < kValueCount; ++i) {
    const uint64_t offset = i * sizeof(uint64_t);
    commands.CopyData64(source->device_address + offset,
                        intermediate->device_address + offset);
    // The first confirmed transfer supplies this dependency. There is no
    // intervening cache command or host completion wait.
    commands.CopyData64(intermediate->device_address + offset,
                        target->device_address + offset);
  }
  commands.SystemBarrier();
  commands.WriteData32(completion->device_address, 1);
  commands.PadToEightWords();
  ASSERT_NO_FATAL_FAILURE(
      queue->Publish(api_, gpu_api_, commands.word_count()));
  GpuWaitEqual<uint32_t>(reinterpret_cast<uintptr_t>(completion->host.pointer),
                         1);
  // Capture every observation before diagnostics or retirement can intervene.
  std::array<uint64_t, kValueCount> observed_staging;
  std::array<uint64_t, kValueCount> observed_output;
  std::array<uint64_t, kValueCount> observed_input;
  std::memcpy(observed_staging.data(), staging, sizeof(observed_staging));
  std::memcpy(observed_output.data(), output, sizeof(observed_output));
  std::memcpy(observed_input.data(), input, sizeof(observed_input));
  for (size_t i = 0; i < kValueCount; ++i) {
    const uint64_t expected =
        UINT64_C(0x13579bdf2468ace0) + i * UINT64_C(0x0102030405060708);
    EXPECT_EQ(observed_staging[i], expected) << i;
    EXPECT_EQ(observed_output[i], expected) << i;
    EXPECT_EQ(observed_input[i], expected) << "source word " << i;
  }
  // Nonfatal oracle failures still reach normal retirement.
  ASSERT_NO_FATAL_FAILURE(queue->WaitRetired(api_));
}

class Pm4DmaTest : public Pm4CommandTest {};

TEST_F(Pm4DmaTest, CoherentSystemCopyCompletesBeforeReuse) {
  constexpr uint32_t kPageByteLength = 4096;
  constexpr uint32_t kPageWordCount = kPageByteLength / sizeof(uint32_t);
  constexpr uint32_t kSourceByteOffset = 64;
  constexpr uint32_t kTargetByteOffset = 256;
  constexpr uint32_t kCopyByteLength = 1024;
  constexpr uint32_t kSourceWordIndex = kSourceByteOffset / sizeof(uint32_t);
  constexpr uint32_t kTargetWordIndex = kTargetByteOffset / sizeof(uint32_t);
  constexpr uint32_t kCopyWordCount = kCopyByteLength / sizeof(uint32_t);
  constexpr uint32_t kCompletionByteOffset = 256;
  constexpr uint32_t kCompletionWordIndex =
      kCompletionByteOffset / sizeof(uint32_t);
  constexpr uint32_t kEpochCount = 2;
  constexpr uint32_t kCommandWordsPerEpoch = 48;
  constexpr uint32_t kCommandWordCount = kEpochCount * kCommandWordsPerEpoch;
  static_assert(kSourceByteOffset + kCopyByteLength <= kPageByteLength);
  static_assert(kTargetByteOffset + kCopyByteLength <= kPageByteLength);

  GpuMemory* source = nullptr;
  GpuMemory* target = nullptr;
  GpuMemory* completion = nullptr;
  ASSERT_NO_FATAL_FAILURE(
      CreateMemory(AMDF_MEMORY_ACCESS_READ, kPageByteLength, &source));
  ASSERT_NO_FATAL_FAILURE(
      CreateMemory(AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE,
                   kPageByteLength, &target));
  ASSERT_NO_FATAL_FAILURE(
      CreateMemory(AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE,
                   kPageByteLength, &completion));
  std::array<uint32_t, kPageWordCount> expected_source, observed_source;
  std::array<uint32_t, kPageWordCount> expected_target, observed_target;
  std::array<uint32_t, kPageWordCount> expected_control, observed_control;
  std::array<uint32_t, kCommandWordCount> expected_commands, observed_commands;
  for (uint32_t i = 0; i < kPageWordCount; ++i) {
    expected_control[i] = 0x68d329b7u ^ i;
  }
  // Only the GPU updates this marker after its one-time host initialization.
  expected_control[kCompletionWordIndex] = 0;
  std::memcpy(completion->host.pointer, expected_control.data(),
              sizeof(expected_control));
  const uintptr_t completion_address =
      reinterpret_cast<uintptr_t>(completion->host.pointer) +
      kCompletionByteOffset;

  GpuCommandQueue* queue = nullptr;
  ASSERT_NO_FATAL_FAILURE(CreateQueue(&queue));
  const uint64_t command_capacity = queue->words().size();
  ASSERT_GT(command_capacity, kCommandWordCount);
  Pm4CommandWriter commands(queue->words().data(), *pm4_profile_);
  // Both batches are complete before the first publication. Each remains
  // immutable through its completion and retired frontier.
  for (uint32_t epoch = 1; epoch <= kEpochCount; ++epoch) {
    commands.SystemBarrier();
    commands.DmaCopyL2(source->device_address + kSourceByteOffset,
                       target->device_address + kTargetByteOffset,
                       kCopyByteLength);
    commands.WaitDma();
    commands.SystemBarrier();
    commands.WriteData32(completion->device_address + kCompletionByteOffset,
                         epoch);
    // The 39-word body requires a complete nine-word NOP, not a one-word gap.
    commands.PadToEightWords();
    ASSERT_EQ(commands.word_count(), epoch * kCommandWordsPerEpoch);
  }
  std::memcpy(expected_commands.data(), queue->words().data(),
              sizeof(expected_commands));
  RecordProperty("pm4_dma_packet_header", "0xc0055000");
  RecordProperty("pm4_dma_copy_control", "0x60300000");
  RecordProperty("pm4_dma_copy_count_control", "0x40000400");
  RecordProperty("pm4_dma_drain_body", "0,0,0,0,0,0");
  RecordProperty("pm4_dma_copy_byte_length", kCopyByteLength);
  RecordProperty("pm4_dma_source_byte_offset", kSourceByteOffset);
  RecordProperty("pm4_dma_target_byte_offset", kTargetByteOffset);
  RecordProperty("pm4_dma_completion_byte_offset", kCompletionByteOffset);
  RecordProperty("pm4_dma_checked_page_bytes_each", kPageByteLength);
  RecordProperty("pm4_dma_checked_allocation_bytes", 3 * kPageByteLength);
  RecordProperty("pm4_dma_checked_command_bytes", sizeof(expected_commands));
  RecordProperty("pm4_dma_command_words_per_epoch", kCommandWordsPerEpoch);
  RecordProperty("pm4_dma_command_word_count", commands.word_count());
  RecordProperty("pm4_dma_command_capacity_dwords",
                 std::to_string(command_capacity));
  RecordProperty("pm4_dma_first_published_word_count", 0);
  RecordProperty("pm4_dma_completed_epochs", 0);

  for (uint32_t epoch = 1; epoch <= kEpochCount; ++epoch) {
    SCOPED_TRACE(epoch);
    for (uint32_t i = 0; i < kPageWordCount; ++i) {
      expected_source[i] = static_cast<uint32_t>(UINT64_C(0x13579bdf) +
                                                 uint64_t{i} * 0x01030507u +
                                                 uint64_t{epoch} * 0x11111111u);
      expected_target[i] = 0x4e90b725u ^ i;
    }
    observed_target = expected_target;
    for (uint32_t i = 0; i < kCopyWordCount; ++i) {
      const uint32_t expected =
          static_cast<uint32_t>(UINT64_C(0x13579bdf) +
                                (uint64_t{kSourceWordIndex} + i) * 0x01030507u +
                                uint64_t{epoch} * 0x11111111u);
      expected_target[kTargetWordIndex + i] = expected;
      observed_target[kTargetWordIndex + i] = ~expected;
    }
    std::memcpy(source->host.pointer, expected_source.data(),
                sizeof(expected_source));
    std::memcpy(target->host.pointer, observed_target.data(),
                sizeof(observed_target));
    expected_control[kCompletionWordIndex] = epoch;
    const uint64_t published_word_count = epoch * kCommandWordsPerEpoch;
    ASSERT_NO_FATAL_FAILURE(
        queue->Publish(api_, gpu_api_, published_word_count));
    GpuWaitEqual<uint32_t>(completion_address, epoch);

    // Capture the complete target first, then all other initialized storage,
    // before diagnostics or retirement can add synchronization.
    std::memcpy(observed_target.data(), target->host.pointer,
                sizeof(observed_target));
    std::memcpy(observed_source.data(), source->host.pointer,
                sizeof(observed_source));
    std::memcpy(observed_control.data(), completion->host.pointer,
                sizeof(observed_control));
    std::memcpy(observed_commands.data(), queue->words().data(),
                sizeof(observed_commands));
    for (uint32_t i = 0; i < kPageWordCount; ++i) {
      EXPECT_EQ(observed_target[i], expected_target[i]) << "target word=" << i;
      EXPECT_EQ(observed_source[i], expected_source[i]) << "source word=" << i;
      EXPECT_EQ(observed_control[i], expected_control[i])
          << "control word=" << i;
    }
    for (uint32_t i = 0; i < kCommandWordCount; ++i) {
      EXPECT_EQ(observed_commands[i], expected_commands[i])
          << "command word=" << i;
    }
    // The marker supplies transfer visibility. Native progress separately
    // retires command storage, including after nonfatal oracle failures.
    EXPECT_NO_FATAL_FAILURE(queue->WaitRetired(api_));
    if (HasFailure()) {
      return;
    }
    RecordProperty(
        "pm4_dma_epoch_" + std::to_string(epoch) + "_published_word_count",
        std::to_string(published_word_count));
    RecordProperty("pm4_dma_completed_epochs", epoch);
  }
  RecordProperty("pm4_dma_final_published_word_count", kCommandWordCount);
}

}  // namespace
