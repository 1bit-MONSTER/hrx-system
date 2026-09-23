// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <algorithm>
#include <array>
#include <vector>

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

TEST_F(SdmaCopyTest, ByteTailsAndPageCrossingsPreserveSurroundingBytes) {
  constexpr std::array<uint32_t, 6> kByteLengths = {1, 2, 3, 4, 31, 4101};
  constexpr uint64_t kSourceLength = 12288;
  constexpr uint64_t kTargetStride = 12288;
  constexpr uint64_t kTargetLength = kTargetStride * kByteLengths.size();
  GpuMemory* source = nullptr;
  GpuMemory* target = nullptr;
  GpuMemory* completion = nullptr;
  ASSERT_NO_FATAL_FAILURE(
      CreateMemory(AMDF_MEMORY_ACCESS_READ, kSourceLength, &source));
  ASSERT_NO_FATAL_FAILURE(
      CreateMemory(AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE,
                   kTargetLength, &target));
  ASSERT_NO_FATAL_FAILURE(CreateMemory(
      AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE, 4096, &completion));
  auto* input = static_cast<uint8_t*>(source->host.pointer);
  auto* output = static_cast<uint8_t*>(target->host.pointer);
  for (uint64_t i = 0; i < kSourceLength; ++i) {
    input[i] = static_cast<uint8_t>(i * 73 + (i >> 8) * 19 + 7);
  }
  std::fill_n(output, kTargetLength, 0xa5);
  std::vector<uint8_t> expected(kTargetLength, 0xa5);
  *static_cast<uint32_t*>(completion->host.pointer) = 0;
  GpuUserQueue* queue = nullptr;
  ASSERT_NO_FATAL_FAILURE(CreateQueue(&queue));
  ASSERT_GE(queue->host.ring_byte_length, 256u);
  SdmaCommandWriter commands(
      reinterpret_cast<uint32_t*>(queue->host.ring_address),
      family_.format_features);
  for (size_t i = 0; i < kByteLengths.size(); ++i) {
    // Differently aligned ranges cross the first source and target page.
    const uint64_t source_offset = i == 3 ? 4092 : 4095;
    const uint64_t target_offset = i * kTargetStride + (i == 3 ? 4092 : 4091);
    commands.CopyLinear(source->device_address + source_offset,
                        target->device_address + target_offset,
                        kByteLengths[i]);
    std::copy_n(input + source_offset, kByteLengths[i],
                expected.data() + target_offset);
  }
  commands.Fence32(completion->device_address, 1);
  const uint64_t byte_length = commands.word_count() * sizeof(uint32_t);
  ASSERT_NO_FATAL_FAILURE(queue->PublishStream(byte_length));
  GpuWaitEqual<uint32_t>(reinterpret_cast<uintptr_t>(completion->host.pointer),
                         1);
  ASSERT_NO_FATAL_FAILURE(queue->WaitConsumed(api_, byte_length));
  for (uint64_t i = 0; i < kTargetLength; ++i) {
    ASSERT_EQ(output[i], expected[i]) << "byte " << i;
  }
  for (uint64_t i = 0; i < kSourceLength; ++i) {
    ASSERT_EQ(input[i], static_cast<uint8_t>(i * 73 + (i >> 8) * 19 + 7))
        << "source byte " << i;
  }
}

}  // namespace
