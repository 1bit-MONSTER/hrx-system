// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <string>

#include "libamdf/cts/gpu/aql/dispatch_fixture.h"
#include "libamdf/cts/gpu/sdma/encoding/commands.h"

namespace {

namespace kernel = kernels::gfx942_transform;

class CopyDispatchRecipeTest : public AqlDispatchTest {
 protected:
  amdf_status_t MatchGpuEndpoint(amdf_endpoint_t* endpoint,
                                 bool* out_matches) override {
    const GpuQueueRequirements requirements = {
        .command_type = AMDF_QUEUE_COMMAND_TYPE_GPU_SDMA,
        .roles = AMDF_QUEUE_ROLE_TRANSFER,
    };
    amdf_queue_family_info_t sdma_family = {};
    bool matches = false;
    amdf_status_t status =
        FindQueueFamily(endpoint, requirements, &sdma_family, &matches);
    if (!amdf_status_is_ok(status)) {
      return status;
    }
    if (!matches || (sdma_family.format_features &
                     AMDF_GPU_SDMA_FORMAT_FEATURE_MEMORY_SCOPE) != 0) {
      *out_matches = false;
      return AMDF_STATUS_OK;
    }
    status = AqlDispatchTest::MatchGpuEndpoint(endpoint, &matches);
    if (!amdf_status_is_ok(status)) {
      return status;
    }
    if (matches) {
      sdma_family_ = sdma_family;
    }
    *out_matches = matches;
    return AMDF_STATUS_OK;
  }

  // Transfer family retained from the same passive endpoint match as AQL.
  amdf_queue_family_info_t sdma_family_ = {};
};

struct alignas(64) CompletionState {
  // Written 1 to 0 by SDMA; AQL depends on the complete native signal block.
  aql::Signal upload;
  // Decremented 1 to 0 by CP; SDMA polls the low word of its value.
  aql::Signal compute;
  // Written only by SDMA after download; one nonzero value per complete epoch.
  alignas(64) uint32_t download;
};

TEST_F(CopyDispatchRecipeTest,
       CoherentSystemUploadDispatchDownloadReusesBothRings) {
  constexpr uint32_t kGridSize = 1024;
  constexpr uint32_t kWordCount = 2048;
  constexpr uint32_t kPayloadOffset = 16;
  constexpr uint32_t kPayloadByteOffset = kPayloadOffset * sizeof(uint32_t);
  constexpr uint32_t kPayloadByteLength = kGridSize * sizeof(uint32_t);
  constexpr uint32_t kSourceGuard = 0x759bf13du;
  constexpr uint32_t kInputGuard = 0x26a4e8c3u;
  constexpr uint32_t kOutputGuard = 0x93b57fd1u;
  constexpr uint32_t kReadbackGuard = 0x4cd218a7u;
  constexpr size_t kChainWordCount = 7 + 4 + 6 + 7 + 4;
  constexpr uint64_t kChainByteLength = kChainWordCount * sizeof(uint32_t);
  constexpr amdf_memory_access_t kReadWrite =
      AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE;
  GpuMemory* source = nullptr;
  GpuMemory* input = nullptr;
  GpuMemory* output = nullptr;
  GpuMemory* readback = nullptr;
  GpuMemory* arguments = nullptr;
  GpuMemory* control = nullptr;
  ASSERT_NO_FATAL_FAILURE(CreateMemory(AMDF_MEMORY_ACCESS_READ,
                                       kWordCount * sizeof(uint32_t), &source));
  ASSERT_NO_FATAL_FAILURE(
      CreateMemory(kReadWrite, kWordCount * sizeof(uint32_t), &input));
  ASSERT_NO_FATAL_FAILURE(
      CreateMemory(kReadWrite, kWordCount * sizeof(uint32_t), &output));
  // KFD mappings always grant GPU read access, including download targets.
  ASSERT_NO_FATAL_FAILURE(
      CreateMemory(kReadWrite, kWordCount * sizeof(uint32_t), &readback));
  ASSERT_NO_FATAL_FAILURE(
      CreateMemory(AMDF_MEMORY_ACCESS_READ, 4096, &arguments));
  ASSERT_NO_FATAL_FAILURE(CreateMemory(kReadWrite, 4096, &control));
  ASSERT_EQ(arguments->device_address % kernel::kKernargAlignment, 0u);
  ASSERT_EQ(control->device_address % alignof(CompletionState), 0u);
  std::memset(control->host.pointer, 0, control->info.byte_length);
  auto& completion = *static_cast<CompletionState*>(control->host.pointer);
  completion.upload.kind = completion.compute.kind = 1;
  const uint64_t upload_signal_address =
      control->device_address + offsetof(CompletionState, upload);
  const uint64_t compute_signal_address =
      control->device_address + offsetof(CompletionState, compute);
  const uint64_t download_address =
      control->device_address + offsetof(CompletionState, download);

  GpuUserQueue* aql_queue = nullptr;
  GpuUserQueue* sdma_queue = nullptr;
  ASSERT_NO_FATAL_FAILURE(CreateQueue(&aql_queue));
  ASSERT_NO_FATAL_FAILURE(CreateQueue(sdma_family_, &sdma_queue));
  ASSERT_TRUE(amdf_device_id_is_equal(&aql_queue->info.device_id,
                                      &sdma_queue->info.device_id));
  ASSERT_FALSE(amdf_queue_id_is_equal(&aql_queue->info.queue_id,
                                      &sdma_queue->info.queue_id));
  const uint64_t aql_capacity =
      aql_queue->host.ring_byte_length / sizeof(aql::Packet);
  const uint64_t sdma_capacity = sdma_queue->host.ring_byte_length;
  ASSERT_GE(aql_capacity, 2u);
  ASSERT_GT(sdma_capacity, 2 * kChainByteLength);
  const uint64_t epoch_count =
      2 * std::max((aql_capacity + 1) / 2, sdma_capacity / kChainByteLength) +
      1;
  ASSERT_LE(epoch_count, UINT32_MAX);
  RecordProperty("copy_dispatch_epoch_count", std::to_string(epoch_count));
  RecordProperty("aql_queue_family_ordinal", family_.ordinal);
  RecordProperty("sdma_queue_family_ordinal", sdma_family_.ordinal);
  RecordProperty("aql_ring_packet_capacity", std::to_string(aql_capacity));
  RecordProperty("sdma_ring_byte_capacity", std::to_string(sdma_capacity));
  uint64_t aql_index = 0;
  uint64_t sdma_index = 0;
  uint64_t descriptor_address = 0;
  ASSERT_NO_FATAL_FAILURE(
      PublishKernel(*aql_queue, &aql_index, &descriptor_address));

  auto* source_words = static_cast<uint32_t*>(source->host.pointer);
  auto* input_words = static_cast<uint32_t*>(input->host.pointer);
  auto* output_words = static_cast<uint32_t*>(output->host.pointer);
  auto* readback_words = static_cast<uint32_t*>(readback->host.pointer);
  auto* sdma_ring = reinterpret_cast<uint8_t*>(sdma_queue->host.ring_address);
  std::array<uint32_t, kWordCount> expected_source;
  std::array<uint32_t, kWordCount> expected_input;
  std::array<uint32_t, kWordCount> expected_output;
  std::array<uint32_t, kWordCount> expected_readback;
  std::array<uint32_t, kWordCount> downloaded;
  for (uint64_t epoch_index = 0; epoch_index < epoch_count; ++epoch_index) {
    const uint32_t epoch = static_cast<uint32_t>(epoch_index + 1);
    const uint32_t count = epoch % 2 == 1 ? 1003 : 997;
    const uint32_t addend = 0x80000001u + 2 * epoch;
    expected_source.fill(kSourceGuard);
    expected_input.fill(kInputGuard);
    expected_output.fill(kOutputGuard);
    expected_readback.fill(kReadbackGuard);
    std::fill_n(source_words, kWordCount, kSourceGuard);
    std::fill_n(input_words, kWordCount, kInputGuard);
    std::fill_n(output_words, kWordCount, kOutputGuard);
    std::fill_n(readback_words, kWordCount, kReadbackGuard);
    for (uint32_t i = 0; i < kGridSize; ++i) {
      const uint32_t position = kPayloadOffset + i;
      const uint32_t value = 0xfffffff0u + i * 0x01030507u + epoch;
      source_words[position] = value;
      input_words[position] = ~value;
      expected_source[position] = expected_input[position] = value;
      if (i < count) {
        // The host oracle is independent of GPU data. Common active lanes
        // change by five per epoch modulo 2^32, including overflowing values.
        const uint32_t result =
            static_cast<uint32_t>(uint64_t{value} * 3 + addend);
        expected_output[position] = result;
        output_words[position] = result ^ 0x5a5a5a5au;
      }
      expected_readback[position] = expected_output[position];
      // Poison tail lanes too: download must copy their untouched guards.
      readback_words[position] = expected_readback[position] ^ 0xa5a5a5a5u;
    }
    const Arguments payload = {
        input->device_address + kPayloadByteOffset,
        output->device_address + kPayloadByteOffset,
        count,
        addend,
    };
    std::memset(arguments->host.pointer, 0, arguments->info.byte_length);
    std::memcpy(arguments->host.pointer, &payload, kernel::kKernargByteLength);
    // Previous final download and both consumed frontiers precede rearming.
    // Only the low word changes on the SDMA upload's finite 1-to-0 transition.
    completion.upload.value = completion.compute.value = 1;
    const auto dependency =
        aql::Barrier(aql::BarrierType::kAnd, aql::HeaderBarrier::kDisabled, 0,
                     {upload_signal_address});
    const auto dispatch = aql::Dispatch1D(
        kernel::kWorkgroupSize, kGridSize, kernel::kPrivateSegmentByteLength,
        kernel::kGroupSegmentByteLength, descriptor_address,
        arguments->device_address, compute_signal_address);
    std::array<uint32_t, kChainWordCount> stream = {};
    SdmaCommandWriter commands(stream.data(), sdma_family_.format_features);
    commands.CopyLinear(source->device_address + kPayloadByteOffset,
                        input->device_address + kPayloadByteOffset,
                        kPayloadByteLength);
    commands.Fence32(upload_signal_address + offsetof(aql::Signal, value), 0);
    commands.WaitMemory32(compute_signal_address + offsetof(aql::Signal, value),
                          0);
    commands.CopyLinear(output->device_address + kPayloadByteOffset,
                        readback->device_address + kPayloadByteOffset,
                        kPayloadByteLength);
    commands.Fence32(download_address, epoch);
    ASSERT_EQ(commands.word_count(), stream.size());
    uint64_t offset = sdma_index % sdma_capacity;
    const uint64_t remaining = sdma_capacity - offset;
    if (remaining < kChainByteLength) {
      // Native zero NOP dwords keep every packet contiguous at ring wrap.
      std::memset(sdma_ring + offset, 0, remaining);
      sdma_index += remaining;
      offset = 0;
    }
    std::memcpy(sdma_ring + offset, stream.data(), kChainByteLength);
    sdma_index += kChainByteLength;

    // Consumer-first publication leaves every dataflow edge on the device.
    // Both rings are free from the preceding epoch before this reservation.
    GpuStoreRelease(aql_queue->host.write_index_address, aql_index + 2);
    Publish(*aql_queue, aql_index++, dependency);
    Publish(*aql_queue, aql_index++, dispatch);
    ASSERT_NO_FATAL_FAILURE(sdma_queue->PublishStream(sdma_index));
    GpuWaitEqual<uint32_t>(reinterpret_cast<uintptr_t>(&completion.download),
                           epoch);

    // Observe the actual download before any native consumption/retirement
    // call or other payload read could maintain this visibility edge for us.
    std::memcpy(downloaded.data(), readback_words, sizeof(downloaded));
    for (uint32_t i = 0; i < kWordCount; ++i) {
      ASSERT_EQ(downloaded[i], expected_readback[i])
          << "epoch=" << epoch << " readback word=" << i;
    }
    for (uint32_t i = 0; i < kWordCount; ++i) {
      ASSERT_EQ(source_words[i], expected_source[i])
          << "epoch=" << epoch << " source word=" << i;
      ASSERT_EQ(input_words[i], expected_input[i])
          << "epoch=" << epoch << " input word=" << i;
      ASSERT_EQ(output_words[i], expected_output[i])
          << "epoch=" << epoch << " output word=" << i;
    }
    ASSERT_EQ(GpuLoadAcquire<int64_t>(
                  reinterpret_cast<uintptr_t>(&completion.upload.value)),
              0);
    ASSERT_EQ(GpuLoadAcquire<int64_t>(
                  reinterpret_cast<uintptr_t>(&completion.compute.value)),
              0);
    ASSERT_NO_FATAL_FAILURE(aql_queue->WaitConsumed(api_, aql_index));
    ASSERT_NO_FATAL_FAILURE(sdma_queue->WaitConsumed(api_, sdma_index));
  }
  ASSERT_GT(aql_index, 2 * aql_capacity);
  ASSERT_GT(sdma_index, 2 * sdma_capacity);
  RecordProperty("copy_dispatch_completed_epochs", std::to_string(epoch_count));
  RecordProperty("aql_final_packet_index", std::to_string(aql_index));
  RecordProperty("sdma_final_byte_index", std::to_string(sdma_index));
}

}  // namespace
