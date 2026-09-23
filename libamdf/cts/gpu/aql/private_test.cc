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
#include "libamdf/cts/gpu/kernels/private_roundtrip_gfx942.h"

namespace {

namespace kernel = kernels::gfx942_private_roundtrip;

struct Arguments {
  // Global GPU address of the first output record, after the prefix guard.
  uint64_t output;
  // Epoch-specific token seed, combined with the global workitem and slot.
  uint32_t seed;
  // Runtime rotation of each workitem's nine private words.
  uint32_t rotation;
};
static_assert(sizeof(Arguments) == kernel::kKernargByteLength);
static_assert(offsetof(Arguments, output) == 0);
static_assert(offsetof(Arguments, seed) == 8);
static_assert(offsetof(Arguments, rotation) == 12);

TEST_F(AqlDispatchTest, CallerOwnedFixedScratchChangesAcrossEpochs) {
  constexpr uint32_t kGridSize = 512;
  constexpr uint32_t kPrivateWordCount = 9;
  constexpr uint32_t kOutputWordCount = kGridSize * kPrivateWordCount;
  constexpr uint32_t kGuardWordCount = 16;
  constexpr uint32_t kWordCount = kOutputWordCount + 2 * kGuardWordCount;
  constexpr uint32_t kPrefixGuard = 0x619b30d5u;
  constexpr uint32_t kSuffixGuard = 0xe270c84bu;
  constexpr std::array<uint32_t, 2> kSeeds = {0x13579bdfu, 0xa5c31f27u};
  constexpr std::array<uint32_t, 2> kRotations = {1, 7};
  constexpr uint64_t kWaveByteLength =
      (uint64_t{kernel::kPrivateSegmentByteLength} * 64 + 1023) &
      ~UINT64_C(1023);
  static_assert(kWaveByteLength == 3072);
  static_assert(kernel::kWorkgroupSize == 64);
  static_assert(kGridSize % kernel::kWorkgroupSize == 0);

  amdf_gpu_endpoint_info_t endpoint_info = {};
  endpoint_info.type = AMDF_STRUCTURE_TYPE_GPU_ENDPOINT_INFO;
  endpoint_info.structure_size = sizeof(endpoint_info);
  ASSERT_EQ(gpu_api_->endpoint_query_info(endpoint_, &endpoint_info),
            AMDF_STATUS_OK);
  const auto& compute = endpoint_info.compute;
  const auto& topology = endpoint_info.topology;
  ASSERT_EQ(compute.wavefront_size, 64u);
  ASSERT_GT(compute.compute_unit_count, 0u);
  ASSERT_GT(compute.maximum_scratch_wave_count_per_compute_unit, 0u);
  ASSERT_GT(topology.xcc_count, 0u);
  ASSERT_GT(topology.shader_engine_count_per_xcc, 0u);
  ASSERT_EQ(compute.compute_unit_count % topology.xcc_count, 0u);
  const uint64_t wave_count =
      uint64_t{compute.compute_unit_count} *
      compute.maximum_scratch_wave_count_per_compute_unit;
  ASSERT_LE(wave_count, UINT32_MAX);
  const uint64_t waves_per_xcc = wave_count / topology.xcc_count;
  ASSERT_LE(waves_per_xcc, 0xfffu);
  ASSERT_EQ(waves_per_xcc % topology.shader_engine_count_per_xcc, 0u);
  const uint64_t bytes_per_xcc = waves_per_xcc * kWaveByteLength;
  ASSERT_LE(bytes_per_xcc, UINT32_MAX);
  // Fixed AQL scratch covers every physical slot; the small grid does not
  // establish which compute units, XCCs or physical slots execute its waves.
  const uint64_t scratch_byte_length = wave_count * kWaveByteLength;
  RecordProperty("aql_scratch_compute_unit_count", compute.compute_unit_count);
  RecordProperty("aql_scratch_slots_per_compute_unit",
                 compute.maximum_scratch_wave_count_per_compute_unit);
  RecordProperty("aql_scratch_shader_engines_per_xcc",
                 topology.shader_engine_count_per_xcc);
  RecordProperty("aql_scratch_physical_wave_capacity",
                 std::to_string(wave_count));
  RecordProperty("aql_scratch_waves_per_xcc", std::to_string(waves_per_xcc));
  RecordProperty("aql_scratch_bytes_per_wave", std::to_string(kWaveByteLength));
  RecordProperty("aql_scratch_bytes_per_xcc", std::to_string(bytes_per_xcc));
  RecordProperty("aql_scratch_byte_length",
                 std::to_string(scratch_byte_length));
  RecordProperty("aql_private_segment_byte_length",
                 kernel::kPrivateSegmentByteLength);
  RecordProperty("aql_private_output_words_per_epoch", kOutputWordCount);

  GpuMemory* scratch_memory = nullptr;
  GpuMemory* output = nullptr;
  GpuMemory* arguments = nullptr;
  GpuMemory* completion = nullptr;
  ASSERT_NO_FATAL_FAILURE(
      CreateMemory(AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE,
                   scratch_byte_length, &scratch_memory));
  ASSERT_EQ(scratch_memory->device_address % 4096, 0u);
  ASSERT_LT(scratch_memory->device_address, UINT64_C(1) << 48);
  ASSERT_LE(scratch_byte_length,
            (UINT64_C(1) << 48) - scratch_memory->device_address);
  ASSERT_NO_FATAL_FAILURE(
      CreateMemory(AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE,
                   uint64_t{kWordCount} * sizeof(uint32_t), &output));
  ASSERT_NO_FATAL_FAILURE(
      CreateMemory(AMDF_MEMORY_ACCESS_READ, 4096, &arguments));
  ASSERT_NO_FATAL_FAILURE(CreateMemory(
      AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE, 4096, &completion));
  ASSERT_EQ(arguments->device_address % kernel::kKernargAlignment, 0u);
  std::memset(completion->host.pointer, 0, completion->info.byte_length);
  auto& signal = *static_cast<aql::Signal*>(completion->host.pointer);
  signal.kind = 1;

  // Scratch stays untouched by the CPU. Each workitem initializes every private
  // word it reads; the queue borrows the full backing until destruction
  // succeeds.
  amdf_gpu_queue_scratch_t scratch = {};
  scratch.memory = scratch_memory->memory;
  scratch.byte_length = scratch_byte_length;
  scratch.maximum_private_segment_byte_length =
      kernel::kPrivateSegmentByteLength;
  scratch.maximum_wave_count = static_cast<uint32_t>(wave_count);
  GpuUserQueue* queue = nullptr;
  ASSERT_NO_FATAL_FAILURE(
      CreateQueue(&queue, AMDF_QUEUE_PRODUCER_MODE_SINGLE, scratch));
  uint64_t index = 0;
  uint64_t descriptor_address = 0;
  ASSERT_NO_FATAL_FAILURE(
      PublishKernel(*queue, kernel::kExecutable, &index, &descriptor_address));

  std::array<uint32_t, kWordCount> expected;
  std::array<uint32_t, kWordCount> observed;
  for (uint32_t epoch = 0; epoch < kSeeds.size(); ++epoch) {
    expected.fill(kSuffixGuard);
    for (uint32_t word = 0; word < kGuardWordCount; ++word) {
      expected[word] = kPrefixGuard;
    }
    for (uint32_t workitem = 0; workitem < kGridSize; ++workitem) {
      const uint32_t local_id = workitem % kernel::kWorkgroupSize;
      uint32_t selected_slot =
          (local_id + kRotations[epoch]) % kPrivateWordCount;
      for (uint32_t word = 0; word < kPrivateWordCount; ++word) {
        // Walk the permutation cyclically, and truncate wider token arithmetic
        // only after forming the complete independent CPU result.
        const uint64_t value = uint64_t{kSeeds[epoch]} +
                               uint64_t{workitem} * 0x01020307u +
                               uint64_t{selected_slot} * 0x1021u;
        expected[kGuardWordCount + workitem * kPrivateWordCount + word] =
            static_cast<uint32_t>(value);
        if (++selected_slot == kPrivateWordCount) {
          selected_slot = 0;
        }
      }
    }
    observed = expected;
    for (uint32_t word = 0; word < kOutputWordCount; ++word) {
      observed[kGuardWordCount + word] = ~expected[kGuardWordCount + word];
    }
    std::memcpy(output->host.pointer, observed.data(), sizeof(observed));
    const Arguments payload = {
        output->device_address + kGuardWordCount * sizeof(uint32_t),
        kSeeds[epoch],
        kRotations[epoch],
    };
    std::memset(arguments->host.pointer, 0, arguments->info.byte_length);
    std::memcpy(arguments->host.pointer, &payload, sizeof(payload));
    ASSERT_EQ(
        GpuLoadAcquire<int64_t>(reinterpret_cast<uintptr_t>(&signal.value)), 0);
    signal.value = 1;
    const auto packet =
        aql::Dispatch({1, {kernel::kWorkgroupSize, 1, 1}, {kGridSize, 1, 1}},
                      kernel::kPrivateSegmentByteLength,
                      kernel::kGroupSegmentByteLength, descriptor_address,
                      arguments->device_address, completion->device_address,
                      {aql::FenceScope::kSystem, aql::FenceScope::kSystem});
    GpuStoreRelease(queue->host.write_index_address, index + 1);
    Publish(*queue, index++, packet);
    // Execution completion and ring consumption precede output observation and
    // the next epoch's signal, kernarg or output reuse.
    ASSERT_NO_FATAL_FAILURE(WaitCompletion(*queue, signal, index));
    std::memcpy(observed.data(), output->host.pointer, sizeof(observed));
    for (uint32_t word = 0; word < kWordCount; ++word) {
      ASSERT_EQ(observed[word], expected[word])
          << "epoch=" << epoch << " word=" << word;
    }
    const std::string prefix = "aql_private_epoch_" + std::to_string(epoch + 1);
    RecordProperty(prefix + "_seed", std::to_string(kSeeds[epoch]));
    RecordProperty(prefix + "_rotation", kRotations[epoch]);
  }
  RecordProperty("aql_private_completed_epochs", kSeeds.size());
}

}  // namespace
