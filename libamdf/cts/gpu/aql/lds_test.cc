// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#include "libamdf/cts/gpu/aql/dispatch_fixture.h"
#include "libamdf/cts/gpu/kernels/lds_exchange_gfx942.h"

namespace {

namespace kernel = kernels::gfx942_lds_exchange;

// Semantic arguments occupy 20 bytes. The aligned slot backs the scalar load
// through byte 23 without making its unused lane another argument.
struct alignas(16) Arguments {
  // Global GPU address of the first output pair, after the prefix guard.
  uint64_t output;
  // Group-segment byte offset of the dynamic region after static LDS.
  uint32_t dynamic_offset;
  // Epoch-specific token seed, combined with workgroup and partner lane.
  uint32_t seed;
  // Dynamic LDS element spacing, or zero to select the static-only branch.
  uint32_t dynamic_stride;
};
static_assert(alignof(Arguments) == kernel::kKernargAlignment);
static_assert(sizeof(Arguments) == 32);
static_assert(offsetof(Arguments, output) == 0);
static_assert(offsetof(Arguments, dynamic_offset) == 8);
static_assert(offsetof(Arguments, seed) == 12);
static_assert(offsetof(Arguments, dynamic_stride) == 16);
static_assert(offsetof(Arguments, dynamic_stride) + sizeof(uint32_t) ==
              kernel::kKernargByteLength);

class AqlLdsTest : public AqlDispatchTest {
 protected:
  void RunExchange(std::array<uint32_t, 2> strides);
};

void AqlLdsTest::RunExchange(std::array<uint32_t, 2> strides) {
  constexpr uint32_t kGridSize = 512;
  constexpr uint32_t kOutputWordCount = kGridSize * 2;
  constexpr uint32_t kGuardWordCount = 16;
  constexpr uint32_t kWordCount = kOutputWordCount + 2 * kGuardWordCount;
  constexpr uint32_t kPrefixGuard = 0x619b30d5u;
  constexpr uint32_t kSuffixGuard = 0xe270c84bu;
  constexpr std::array<uint32_t, 2> kSeeds = {0x13579bdfu, 0xa5c31f27u};
  static_assert(kernel::kWorkgroupSize == 128);
  static_assert(kernel::kGroupSegmentByteLength == 512);
  static_assert(kernel::kPrivateSegmentByteLength == 0);
  static_assert(kGridSize / kernel::kWorkgroupSize == 4);
  static_assert(kGridSize % kernel::kWorkgroupSize == 0);

  amdf_gpu_endpoint_info_t endpoint_info = {};
  endpoint_info.type = AMDF_STRUCTURE_TYPE_GPU_ENDPOINT_INFO;
  endpoint_info.structure_size = sizeof(endpoint_info);
  ASSERT_EQ(gpu_api_->endpoint_query_info(endpoint_, &endpoint_info),
            AMDF_STATUS_OK);
  ASSERT_EQ(endpoint_info.compute.wavefront_size, 64u);
  std::array<uint32_t, 2> group_byte_lengths;
  for (uint32_t epoch = 0; epoch < strides.size(); ++epoch) {
    const uint64_t group_byte_length =
        uint64_t{kernel::kGroupSegmentByteLength} +
        uint64_t{kernel::kWorkgroupSize} * strides[epoch] * sizeof(uint32_t);
    ASSERT_LE(group_byte_length, UINT32_MAX);
    ASSERT_LE(group_byte_length,
              endpoint_info.compute.local_data_share_byte_length);
    group_byte_lengths[epoch] = static_cast<uint32_t>(group_byte_length);
  }
  RecordProperty(
      "aql_lds_capacity_per_compute_unit",
      std::to_string(endpoint_info.compute.local_data_share_byte_length));
  RecordProperty("aql_lds_fixed_group_byte_length",
                 kernel::kGroupSegmentByteLength);
  RecordProperty("aql_lds_dynamic_byte_offset",
                 kernel::kGroupSegmentByteLength);
  RecordProperty("aql_lds_kernarg_semantic_byte_length",
                 kernel::kKernargByteLength);
  RecordProperty("aql_lds_kernarg_slot_byte_length", sizeof(Arguments));
  RecordProperty("aql_lds_workgroup_size", kernel::kWorkgroupSize);
  RecordProperty("aql_lds_grid_size", kGridSize);
  RecordProperty("aql_lds_output_words_per_epoch", kOutputWordCount);

  GpuMemory* output = nullptr;
  GpuMemory* arguments = nullptr;
  GpuMemory* completion = nullptr;
  ASSERT_NO_FATAL_FAILURE(
      CreateMemory(AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE,
                   uint64_t{kWordCount} * sizeof(uint32_t), &output));
  ASSERT_NO_FATAL_FAILURE(
      CreateMemory(AMDF_MEMORY_ACCESS_READ, 4096, &arguments));
  ASSERT_GE(arguments->info.byte_length, sizeof(Arguments));
  ASSERT_EQ(arguments->device_address % kernel::kKernargAlignment, 0u);
  ASSERT_NO_FATAL_FAILURE(CreateMemory(
      AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE, 4096, &completion));
  std::memset(completion->host.pointer, 0, completion->info.byte_length);
  auto& signal = *static_cast<aql::Signal*>(completion->host.pointer);
  signal.kind = 1;

  GpuUserQueue* queue = nullptr;
  ASSERT_NO_FATAL_FAILURE(CreateQueue(&queue));
  uint64_t index = 0;
  uint64_t descriptor_address = 0;
  ASSERT_NO_FATAL_FAILURE(
      PublishKernel(*queue, kernel::kExecutable, &index, &descriptor_address));

  std::array<uint32_t, kWordCount> expected;
  std::array<uint32_t, kWordCount> observed;
  for (uint32_t epoch = 0; epoch < strides.size(); ++epoch) {
    expected.fill(kSuffixGuard);
    for (uint32_t word = 0; word < kGuardWordCount; ++word) {
      expected[word] = kPrefixGuard;
    }
    for (uint32_t workitem = 0; workitem < kGridSize; ++workitem) {
      const uint32_t group = workitem / kernel::kWorkgroupSize;
      const uint32_t lane = workitem % kernel::kWorkgroupSize;
      const uint32_t partner = lane < 64 ? lane + 64 : lane - 64;
      // Derive the other wave's lane independently, form its tokens with wider
      // arithmetic, then apply the kernel's unsigned 32-bit wrapping.
      const uint64_t static_value = uint64_t{kSeeds[epoch]} +
                                    uint64_t{group} * 0x01020307u +
                                    uint64_t{partner} * 0x1021u;
      const uint32_t position = kGuardWordCount + workitem * 2;
      expected[position] = static_cast<uint32_t>(static_value);
      if (strides[epoch] != 0) {
        const uint64_t dynamic_value = uint64_t{kSeeds[epoch] ^ 0xa5a55a5au} +
                                       uint64_t{group} * 0x01010101u +
                                       uint64_t{partner} * 0x0203u +
                                       uint64_t{strides[epoch]} * 0x00100001u;
        expected[position + 1] = static_cast<uint32_t>(dynamic_value);
      } else {
        expected[position + 1] =
            kSeeds[epoch] ^
            static_cast<uint32_t>(uint64_t{0x5a17c0deu} + workitem);
      }
    }
    observed = expected;
    for (uint32_t word = 0; word < kOutputWordCount; ++word) {
      observed[kGuardWordCount + word] = ~expected[kGuardWordCount + word];
    }
    std::memcpy(output->host.pointer, observed.data(), sizeof(observed));
    const Arguments payload = {
        output->device_address + kGuardWordCount * sizeof(uint32_t),
        kernel::kGroupSegmentByteLength,
        kSeeds[epoch],
        strides[epoch],
    };
    // The fourth fetched dword has backing and a defined value, while only the
    // first 20 semantic bytes come from the typed argument fields.
    std::memset(arguments->host.pointer, 0, sizeof(Arguments));
    std::memcpy(arguments->host.pointer, &payload, kernel::kKernargByteLength);
    ASSERT_EQ(
        GpuLoadAcquire<int64_t>(reinterpret_cast<uintptr_t>(&signal.value)), 0);
    signal.value = 1;
    const auto packet =
        aql::Dispatch({1, {kernel::kWorkgroupSize, 1, 1}, {kGridSize, 1, 1}},
                      kernel::kPrivateSegmentByteLength,
                      group_byte_lengths[epoch], descriptor_address,
                      arguments->device_address, completion->device_address,
                      {aql::FenceScope::kSystem, aql::FenceScope::kSystem});
    GpuStoreRelease(queue->host.write_index_address, index + 1);
    Publish(*queue, index++, packet);
    // Execution completion and ring consumption precede output observation and
    // every reuse of the signal, kernargs and output in the next epoch.
    ASSERT_NO_FATAL_FAILURE(WaitCompletion(*queue, signal, index));
    std::memcpy(observed.data(), output->host.pointer, sizeof(observed));
    for (uint32_t word = 0; word < kWordCount; ++word) {
      ASSERT_EQ(observed[word], expected[word])
          << "epoch=" << epoch << " word=" << word;
    }
    const std::string prefix = "aql_lds_epoch_" + std::to_string(epoch + 1);
    RecordProperty(prefix + "_seed", std::to_string(kSeeds[epoch]));
    RecordProperty(prefix + "_dynamic_stride", strides[epoch]);
    RecordProperty(prefix + "_group_byte_length", group_byte_lengths[epoch]);
  }
  RecordProperty("aql_lds_completed_epochs", strides.size());
}

TEST_F(AqlLdsTest, StaticStorageExchangesBetweenWaves) {
  ASSERT_NO_FATAL_FAILURE(RunExchange({0, 0}));
}

TEST_F(AqlLdsTest, DynamicStorageExchangesBetweenWavesAcrossSizes) {
  ASSERT_NO_FATAL_FAILURE(RunExchange({1, 3}));
}

}  // namespace
