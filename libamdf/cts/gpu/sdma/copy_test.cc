// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "libamdf/cts/gpu/sdma/encoding/commands.h"
#include "libamdf/cts/gpu/util/command_fixture.h"

namespace {

class SdmaCopyTest : public GpuCommandTest {
 protected:
  SdmaCopyTest()
      : GpuCommandTest(AMDF_QUEUE_COMMAND_TYPE_GPU_SDMA,
                       AMDF_QUEUE_ROLE_TRANSFER) {}
};

TEST_F(SdmaCopyTest, LinearCopyCompletesBeforeFence) {
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
  constexpr size_t kWordCount = 257;
  for (size_t i = 0; i < kWordCount; ++i) {
    input[i] = 0x2ac40000u + static_cast<uint32_t>(i) * 0x00010301u;
    output[i] = ~input[i];
  }
  // Guard words make an incorrect byte count observable.
  output[kWordCount] = 0x725ae191;
  GpuUserQueue* queue = nullptr;
  ASSERT_NO_FATAL_FAILURE(CreateQueue(&queue));
  ASSERT_GE(queue->host.ring_byte_length, 64u);
  SdmaCommandWriter commands(
      reinterpret_cast<uint32_t*>(queue->host.ring_address),
      family_.format_features);
  commands.CopyLinear(source->device_address, target->device_address,
                      kWordCount * sizeof(uint32_t));
  commands.Fence32(completion->device_address, 1);
  const uint64_t byte_length = commands.word_count() * sizeof(uint32_t);
  ASSERT_NO_FATAL_FAILURE(queue->PublishStream(byte_length));
  GpuWaitEqual<uint32_t>(reinterpret_cast<uintptr_t>(completion->host.pointer),
                         1);
  ASSERT_NO_FATAL_FAILURE(queue->WaitConsumed(api_, byte_length));
  for (size_t i = 0; i < kWordCount; ++i) {
    EXPECT_EQ(output[i], input[i]) << i;
  }
  EXPECT_EQ(output[kWordCount], 0x725ae191u);
}

}  // namespace
