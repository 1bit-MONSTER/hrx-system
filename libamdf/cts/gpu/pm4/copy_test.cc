// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "libamdf/cts/gpu/pm4/encoding/commands.h"
#include "libamdf/cts/gpu/util/command_fixture.h"

namespace {

class Pm4CopyTest : public GpuCommandTest {
 protected:
  Pm4CopyTest()
      : GpuCommandTest(AMDF_QUEUE_COMMAND_TYPE_GPU_PM4,
                       AMDF_QUEUE_ROLE_TRANSFER | AMDF_QUEUE_ROLE_CACHE_CONTROL,
                       AMDF_GPU_PM4_FORMAT_FEATURE_ACQUIRE_MEM_GCR) {}
};

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
  ASSERT_GE(queue->host.ring_byte_length, 512u);
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
  ASSERT_NO_FATAL_FAILURE(queue->WaitConsumed(api_, commands.word_count()));
  for (size_t i = 0; i < kWordCount; ++i) {
    EXPECT_EQ(output[i], input[i]) << i;
  }
}

}  // namespace
