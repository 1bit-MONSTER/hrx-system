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

class Pm4WriteTest : public Pm4CommandTest,
                     public ::testing::WithParamInterface<size_t> {};

TEST_P(Pm4WriteTest, WritesIncrementingPayloadAndPreservesGuards) {
  constexpr size_t kWordCount = 4096 / sizeof(uint32_t);
  constexpr size_t kFirstWord = 15;
  const size_t value_count = GetParam();
  std::array<uint32_t, 65> values;
  GpuMemory* target = nullptr;
  GpuMemory* completion = nullptr;
  ASSERT_NO_FATAL_FAILURE(CreateMemory(
      AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE, 4096, &target));
  ASSERT_NO_FATAL_FAILURE(CreateMemory(
      AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE, 4096, &completion));
  auto* output = static_cast<uint32_t*>(target->host.pointer);
  *static_cast<uint32_t*>(completion->host.pointer) = 0;
  GpuCommandQueue* queue = nullptr;
  ASSERT_NO_FATAL_FAILURE(CreateQueue(&queue));
  ASSERT_GE(queue->words().size_bytes(), 1024u);
  Pm4CommandWriter commands(queue->words().data(), *pm4_profile_);
  for (uint32_t epoch = 0; epoch < 2; ++epoch) {
    for (size_t i = 0; i < values.size(); ++i) {
      const uint32_t value =
          i == 0   ? 0
          : i == 1 ? UINT32_MAX
                   : 0x13579bdfu + static_cast<uint32_t>(i) * 0x10203041u;
      values[i] = value ^ (epoch * 0x7139b25du);
    }
    for (size_t i = 0; i < kWordCount; ++i) {
      output[i] = 0xa5a50000u ^ static_cast<uint32_t>(i) ^ epoch;
    }
    commands.SystemBarrier();
    commands.WriteData(target->device_address + kFirstWord * sizeof(uint32_t),
                       values.data(), value_count);
    commands.SystemBarrier();
    commands.WriteData32(completion->device_address, epoch + 1);
    commands.PadToEightWords();
    ASSERT_NO_FATAL_FAILURE(
        queue->Publish(api_, gpu_api_, commands.word_count()));
    GpuWaitEqual<uint32_t>(
        reinterpret_cast<uintptr_t>(completion->host.pointer), epoch + 1);
    // Capture every observation before diagnostics or retirement intervene.
    std::array<uint32_t, kWordCount> observed_output;
    std::memcpy(observed_output.data(), output, sizeof(observed_output));
    for (size_t i = 0; i < kWordCount; ++i) {
      const uint32_t expected =
          i >= kFirstWord && i < kFirstWord + value_count
              ? values[i - kFirstWord]
              : 0xa5a50000u ^ static_cast<uint32_t>(i) ^ epoch;
      EXPECT_EQ(observed_output[i], expected)
          << "epoch=" << epoch << " word=" << i;
    }
    // Retire the accepted command range even after a payload mismatch. The
    // next publication appends commands rather than replaying the first range.
    ASSERT_NO_FATAL_FAILURE(queue->WaitRetired(api_));
    if (HasFailure()) {
      return;
    }
  }
}

INSTANTIATE_TEST_SUITE_P(PayloadWords, Pm4WriteTest,
                         ::testing::Values(1, 2, 3, 17, 65),
                         [](const ::testing::TestParamInfo<size_t>& info) {
                           return "Words" + std::to_string(info.param);
                         });

}  // namespace
