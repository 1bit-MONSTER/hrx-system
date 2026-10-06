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

class Pm4TimestampTest : public Pm4CommandTest {};

TEST_F(Pm4TimestampTest, CommandProcessorSamplesBracketConfirmedCopies) {
  constexpr size_t kValueCount = 16;
  constexpr uint64_t kGuard = UINT64_C(0x13579bdf2468ace0);
  GpuMemory* source = nullptr;
  GpuMemory* target = nullptr;
  GpuMemory* observations = nullptr;
  ASSERT_NO_FATAL_FAILURE(CreateMemory(AMDF_MEMORY_ACCESS_READ, 4096, &source));
  ASSERT_NO_FATAL_FAILURE(CreateMemory(
      AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE, 4096, &target));
  ASSERT_NO_FATAL_FAILURE(CreateMemory(
      AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE, 4096, &observations));
  auto* input = static_cast<uint64_t*>(source->host.pointer);
  auto* output = static_cast<uint64_t*>(target->host.pointer);
  auto* samples = static_cast<uint64_t*>(observations->host.pointer);
  for (size_t i = 0; i < kValueCount; ++i) {
    input[i] = kGuard + i * UINT64_C(0x0102030405060708);
    output[i] = ~input[i];
  }
  for (size_t i = 0; i < 16; ++i) {
    samples[i] = kGuard;
  }
  // Asymmetric poison makes a partial-width timestamp store reverse the
  // observed order when the samples share the same upper clock word.
  samples[1] = UINT64_MAX;
  samples[9] = 0;
  samples[32] = 0;
  GpuCommandQueue* queue = nullptr;
  ASSERT_NO_FATAL_FAILURE(CreateQueue(&queue));
  ASSERT_GE(queue->words().size_bytes(), 1024u);
  Pm4CommandWriter commands(queue->words().data(), *pm4_profile_);
  commands.SystemBarrier();
  commands.CopyGpuClock64(observations->device_address + 8);
  for (size_t i = 0; i < kValueCount; ++i) {
    commands.CopyData64(source->device_address + i * sizeof(uint64_t),
                        target->device_address + i * sizeof(uint64_t));
  }
  commands.CopyGpuClock64(observations->device_address + 72);
  commands.SystemBarrier();
  // A separate completion protects every output and timestamp read. Sampling
  // the CP clock does not itself complete shader work or release payload
  // caches.
  commands.WriteData32(observations->device_address + 256, 1);
  commands.PadToEightWords();
  ASSERT_NO_FATAL_FAILURE(
      queue->Publish(api_, gpu_api_, commands.word_count()));
  GpuWaitEqual<uint32_t>(reinterpret_cast<uintptr_t>(samples + 32), 1);
  // Capture every observation before diagnostics or retirement can intervene.
  std::array<uint64_t, kValueCount> observed_output;
  std::array<uint64_t, kValueCount> observed_input;
  std::array<uint64_t, 16> observed_samples;
  std::memcpy(observed_output.data(), output, sizeof(observed_output));
  std::memcpy(observed_input.data(), input, sizeof(observed_input));
  std::memcpy(observed_samples.data(), samples, sizeof(observed_samples));
  for (size_t i = 0; i < kValueCount; ++i) {
    const uint64_t expected = kGuard + i * UINT64_C(0x0102030405060708);
    EXPECT_EQ(observed_output[i], expected) << i;
    EXPECT_EQ(observed_input[i], expected) << "source word " << i;
  }
  for (size_t i = 0; i < 16; ++i) {
    if (i == 1) {
      EXPECT_NE(observed_samples[i], UINT64_MAX) << i;
    } else if (i == 9) {
      EXPECT_NE(observed_samples[i], 0u) << i;
    } else {
      EXPECT_EQ(observed_samples[i], kGuard) << i;
    }
  }
  // Equal samples are legal at the clock's resolution. This finite run assumes
  // no counter wrap and makes no tick-frequency or elapsed-time claim.
  EXPECT_LE(observed_samples[1], observed_samples[9]);
  RecordProperty("cp_gpu_clock_begin_ticks",
                 std::to_string(observed_samples[1]));
  RecordProperty("cp_gpu_clock_end_ticks", std::to_string(observed_samples[9]));
  // Nonfatal oracle failures still reach normal retirement.
  ASSERT_NO_FATAL_FAILURE(queue->WaitRetired(api_));
}

}  // namespace
