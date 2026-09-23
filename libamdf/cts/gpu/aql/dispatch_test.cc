// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <array>
#include <cstring>

#include "libamdf/cts/gpu/aql/dispatch_fixture.h"
#include "libamdf/cts/gpu/kernels/transform.h"
#include "libamdf/cts/gpu/kernels/transform_gfx942.h"

namespace {

namespace kernel = kernels::gfx942_transform;

TEST_F(AqlDispatchTest, CoherentSystemPayloadChangesAcrossEpochs) {
  constexpr uint32_t kGridSize = 1024;
  constexpr uint32_t kWordCount = 2048;
  constexpr uint32_t kPayloadOffset = 16;
  constexpr uint32_t kGuard = 0x759bf13du;
  constexpr std::array<uint32_t, 2> kCounts = {1003, 997};
  constexpr std::array<uint32_t, 2> kAddends = {7, 0x80000023u};
  GpuMemory* input = nullptr;
  GpuMemory* output = nullptr;
  GpuMemory* arguments = nullptr;
  GpuMemory* completion = nullptr;
  ASSERT_NO_FATAL_FAILURE(CreateMemory(AMDF_MEMORY_ACCESS_READ,
                                       kWordCount * sizeof(uint32_t), &input));
  ASSERT_NO_FATAL_FAILURE(
      CreateMemory(AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE,
                   kWordCount * sizeof(uint32_t), &output));
  ASSERT_NO_FATAL_FAILURE(
      CreateMemory(AMDF_MEMORY_ACCESS_READ, 4096, &arguments));
  ASSERT_NO_FATAL_FAILURE(CreateMemory(
      AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE, 4096, &completion));
  ASSERT_EQ(arguments->device_address % kernel::kKernargAlignment, 0u);
  std::memset(completion->host.pointer, 0, completion->info.byte_length);
  auto& signal = *static_cast<aql::Signal*>(completion->host.pointer);
  signal.kind = 1;

  GpuUserQueue* queue = nullptr;
  ASSERT_NO_FATAL_FAILURE(CreateQueue(&queue));
  uint64_t index = 0;
  uint64_t descriptor_address = 0;
  ASSERT_NO_FATAL_FAILURE(
      PublishKernel(*queue, kernel::kExecutable, &index, &descriptor_address));

  for (uint32_t epoch = 0; epoch < kCounts.size(); ++epoch) {
    std::array<uint32_t, kWordCount> upload;
    std::array<uint32_t, kWordCount> expected;
    std::array<uint32_t, kWordCount> download;
    std::array<uint32_t, kWordCount> unchanged_input;
    upload.fill(kGuard);
    expected.fill(kGuard);
    download.fill(kGuard);
    for (uint32_t i = 0; i < kGridSize; ++i) {
      const uint32_t value =
          0xfffffff0u + i * 0x01030507u + epoch * 0x11111111u;
      upload[kPayloadOffset + i] = value;
      if (i < kCounts[epoch]) {
        // The CPU oracle uses wider arithmetic, then applies uint32 wrapping.
        const uint32_t result =
            static_cast<uint32_t>(uint64_t{value} * 3 + kAddends[epoch]);
        expected[kPayloadOffset + i] = result;
        download[kPayloadOffset + i] = ~result;
      }
    }
    std::memcpy(input->host.pointer, upload.data(), sizeof(upload));
    std::memcpy(output->host.pointer, download.data(), sizeof(download));
    const kernels::transform::Arguments payload = {
        input->device_address + kPayloadOffset * sizeof(uint32_t),
        output->device_address + kPayloadOffset * sizeof(uint32_t),
        kCounts[epoch],
        kAddends[epoch],
    };
    std::memset(arguments->host.pointer, 0, arguments->info.byte_length);
    std::memcpy(arguments->host.pointer, &payload, kernel::kKernargByteLength);
    signal.value = 1;
    const auto packet =
        aql::Dispatch(aql::HeaderBarrier::kDisabled,
                      {1, {kernel::kWorkgroupSize, 1, 1}, {kGridSize, 1, 1}},
                      kernel::kPrivateSegmentByteLength,
                      kernel::kGroupSegmentByteLength, descriptor_address,
                      arguments->device_address, completion->device_address);
    GpuStoreRelease(queue->host.write_index_address, index + 1);
    Publish(*queue, index++, packet);
    // Snapshot completion-visible data before diagnostics or ring retirement.
    GpuWaitEqual<int64_t>(reinterpret_cast<uintptr_t>(&signal.value), 0);
    std::memcpy(download.data(), output->host.pointer, sizeof(download));
    std::memcpy(unchanged_input.data(), input->host.pointer,
                sizeof(unchanged_input));
    for (uint32_t i = 0; i < kWordCount; ++i) {
      EXPECT_EQ(download[i], expected[i]) << "epoch=" << epoch << " word=" << i;
      EXPECT_EQ(unchanged_input[i], upload[i])
          << "epoch=" << epoch << " word=" << i;
    }
    // Retire even after a failed oracle, before any next-epoch storage reuse.
    EXPECT_NO_FATAL_FAILURE(queue->WaitConsumed(api_, index));
    if (HasFailure()) {
      return;
    }
  }
  RecordProperty("aql_payload_completed_epochs", kCounts.size());
}

}  // namespace
