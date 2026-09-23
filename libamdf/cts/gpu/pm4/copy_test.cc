// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <array>
#include <cstring>

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

  GpuUserQueue* queue = nullptr;
  ASSERT_NO_FATAL_FAILURE(CreateQueue(&queue));
  EXPECT_TRUE(amdf_device_id_is_equal(&queue->info.device_id,
                                      &source->access_info.device_id));
  EXPECT_TRUE(amdf_device_id_is_equal(&queue->info.device_id,
                                      &target->access_info.device_id));
  ASSERT_GT(queue->host.ring_byte_length, 512u);
  Pm4CommandWriter commands(
      reinterpret_cast<uint32_t*>(queue->host.ring_address));
  commands.SystemBarrier();
  for (size_t i = 0; i < kWordCount; ++i) {
    commands.CopyData32(source->device_address + i * sizeof(uint32_t),
                        target->device_address + i * sizeof(uint32_t));
  }
  commands.SystemBarrier();
  commands.WriteData32(completion->device_address, 1);
  commands.PadToEightWords();
  ASSERT_NO_FATAL_FAILURE(queue->PublishStream(commands.word_count()));
  GpuWaitEqual<uint32_t>(reinterpret_cast<uintptr_t>(completion->host.pointer),
                         1);
  // Capture every observation before diagnostics or consumption can intervene.
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
  ASSERT_NO_FATAL_FAILURE(queue->WaitConsumed(api_, commands.word_count()));
}

class Pm4CopyWidthTest : public Pm4CommandTest,
                         public ::testing::WithParamInterface<size_t> {};

TEST_P(Pm4CopyWidthTest, PreservesAllWordsOutsideSelectedTransfers) {
  constexpr size_t kWordCount = 4096 / sizeof(uint32_t);
  const size_t copy_byte_length = GetParam();
  const std::array<size_t, 4> positions =
      copy_byte_length == 4 ? std::array<size_t, 4>{1, 15, 17, kWordCount - 1}
                            : std::array<size_t, 4>{2, 14, 16, kWordCount - 2};
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
  std::array<uint32_t, kWordCount> expected;
  for (size_t i = 0; i < kWordCount; ++i) {
    input[i] = 0x13579bdfu + static_cast<uint32_t>(i) * 0x10203041u;
    expected[i] = output[i] = ~input[i];
  }

  GpuUserQueue* queue = nullptr;
  ASSERT_NO_FATAL_FAILURE(CreateQueue(&queue));
  ASSERT_GE(queue->host.ring_byte_length, 256u);
  Pm4CommandWriter commands(
      reinterpret_cast<uint32_t*>(queue->host.ring_address));
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
  ASSERT_NO_FATAL_FAILURE(queue->PublishStream(commands.word_count()));
  GpuWaitEqual<uint32_t>(reinterpret_cast<uintptr_t>(completion->host.pointer),
                         1);
  // Capture every observation before diagnostics or consumption can intervene.
  std::array<uint32_t, kWordCount> observed_output;
  std::array<uint32_t, kWordCount> observed_input;
  std::memcpy(observed_output.data(), output, sizeof(observed_output));
  std::memcpy(observed_input.data(), input, sizeof(observed_input));
  for (size_t i = 0; i < kWordCount; ++i) {
    EXPECT_EQ(observed_output[i], expected[i]) << i;
    const uint32_t expected_input =
        0x13579bdfu + static_cast<uint32_t>(i) * 0x10203041u;
    EXPECT_EQ(observed_input[i], expected_input) << "source word " << i;
  }
  // Nonfatal oracle failures still reach normal retirement.
  ASSERT_NO_FATAL_FAILURE(queue->WaitConsumed(api_, commands.word_count()));
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
  GpuUserQueue* queue = nullptr;
  ASSERT_NO_FATAL_FAILURE(CreateQueue(&queue));
  ASSERT_GE(queue->host.ring_byte_length, 1024u);
  Pm4CommandWriter commands(
      reinterpret_cast<uint32_t*>(queue->host.ring_address));
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
  ASSERT_NO_FATAL_FAILURE(queue->PublishStream(commands.word_count()));
  GpuWaitEqual<uint32_t>(reinterpret_cast<uintptr_t>(completion->host.pointer),
                         1);
  // Capture every observation before diagnostics or consumption can intervene.
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
  ASSERT_NO_FATAL_FAILURE(queue->WaitConsumed(api_, commands.word_count()));
}

}  // namespace
