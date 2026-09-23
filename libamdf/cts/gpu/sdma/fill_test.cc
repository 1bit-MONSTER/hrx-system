// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <array>
#include <cstring>

#include "libamdf/cts/gpu/sdma/encoding/commands.h"
#include "libamdf/cts/gpu/util/command_fixture.h"

namespace {

class SdmaFillTest : public GpuCommandTest {
 protected:
  SdmaFillTest()
      : GpuCommandTest(AMDF_QUEUE_COMMAND_TYPE_GPU_SDMA,
                       AMDF_QUEUE_ROLE_TRANSFER) {}

  amdf_status_t MatchGpuEndpoint(amdf_endpoint_t* endpoint,
                                 bool* out_matches) override {
    bool matches = false;
    const amdf_status_t status =
        GpuCommandTest::MatchGpuEndpoint(endpoint, &matches);
    if (!amdf_status_is_ok(status)) {
      return status;
    }
    // This fixed fill layout excludes newer fence and scope encodings before
    // device activation. Native SDMA selection belongs to the family provider.
    constexpr amdf_queue_format_features_t kExcludedFeatures =
        AMDF_GPU_SDMA_FORMAT_FEATURE_FENCE_SYSTEM |
        AMDF_GPU_SDMA_FORMAT_FEATURE_MEMORY_SCOPE;
    *out_matches =
        matches && (family_.format_features & kExcludedFeatures) == 0;
    return AMDF_STATUS_OK;
  }
};

TEST_F(SdmaFillTest, ConstantFillCompletesBeforeFence) {
  constexpr size_t kTargetLength = 8192;
  constexpr size_t kCompletionLength = 4096;
  constexpr uint8_t kPoison = 0xa5;
  constexpr uint8_t kCompletionGuard = 0x5d;
  struct FillSpan {
    // Byte offset within the owned target allocation.
    uint32_t offset;
    // Nonzero DWORD-aligned byte length.
    uint32_t length;
    // Full 32-bit pattern repeated in little-endian order.
    uint32_t pattern;
  };
  constexpr std::array<FillSpan, 3> kFills = {
      {{64, 4, 0x6d2ac491u}, {128, 8, 0xb730e85au}, {4092, 1028, 0x1fe43962u}}};
  GpuMemory* target = nullptr;
  GpuMemory* completion = nullptr;
  // The native SYSTEM profile guarantees READ permission, so both writable
  // attachments request the complete READ|WRITE contract.
  ASSERT_NO_FATAL_FAILURE(
      CreateMemory(AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE,
                   kTargetLength, &target));
  ASSERT_NO_FATAL_FAILURE(
      CreateMemory(AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE,
                   kCompletionLength, &completion));
  ASSERT_EQ(target->device_address % 4096, 0u);
  ASSERT_EQ(completion->device_address % sizeof(uint32_t), 0u);
  std::memset(target->host.pointer, kPoison, kTargetLength);
  std::memset(completion->host.pointer, kCompletionGuard, kCompletionLength);
  *static_cast<uint32_t*>(completion->host.pointer) = 0;

  std::array<uint8_t, kTargetLength> expected_target;
  std::array<uint8_t, kCompletionLength> expected_completion;
  expected_target.fill(kPoison);
  expected_completion.fill(kCompletionGuard);
  for (const auto& fill : kFills) {
    for (uint32_t byte = 0; byte < fill.length; ++byte) {
      expected_target[fill.offset + byte] =
          static_cast<uint8_t>(fill.pattern >> (8 * (byte & 3)));
    }
  }
  // The completion oracle includes the entire guarded allocation, exposing
  // writes wider than the independent 32-bit fence marker.
  for (size_t byte = 0; byte < sizeof(uint32_t); ++byte) {
    expected_completion[byte] = 0;
  }
  expected_completion[0] = 1;

  GpuUserQueue* queue = nullptr;
  ASSERT_NO_FATAL_FAILURE(CreateQueue(&queue));
  ASSERT_GE(queue->host.ring_byte_length, 19u * sizeof(uint32_t));
  SdmaCommandWriter commands(
      reinterpret_cast<uint32_t*>(queue->host.ring_address),
      family_.format_features);
  // The three disjoint fills need no ordering among themselves. The final
  // range crosses a mapped page boundary while remaining inside the target.
  for (const auto& fill : kFills) {
    commands.Fill32(target->device_address + fill.offset, fill.pattern,
                    fill.length);
  }
  commands.Fence32(completion->device_address, 1);
  ASSERT_EQ(commands.word_count(), 19u);
  const uint64_t byte_length = commands.word_count() * sizeof(uint32_t);
  ASSERT_NO_FATAL_FAILURE(queue->PublishStream(byte_length));
  GpuWaitEqual<uint32_t>(reinterpret_cast<uintptr_t>(completion->host.pointer),
                         1);

  // Snapshot and observe both complete allocations before ring consumption
  // can add synchronization. No payload cache operation intervenes.
  std::array<uint8_t, kTargetLength> observed_target;
  std::array<uint8_t, kCompletionLength> observed_completion;
  std::memcpy(observed_target.data(), target->host.pointer,
              observed_target.size());
  std::memcpy(observed_completion.data(), completion->host.pointer,
              observed_completion.size());
  for (size_t byte = 0; byte < expected_target.size(); ++byte) {
    EXPECT_EQ(observed_target[byte], expected_target[byte])
        << "target byte=" << byte;
  }
  for (size_t byte = 0; byte < expected_completion.size(); ++byte) {
    EXPECT_EQ(observed_completion[byte], expected_completion[byte])
        << "completion byte=" << byte;
  }
  // Nonfatal oracle failures still reach retirement. The fixture removes the
  // queue before releasing either allocation, preserving backing on failure.
  ASSERT_NO_FATAL_FAILURE(queue->WaitConsumed(api_, byte_length));
}

}  // namespace
