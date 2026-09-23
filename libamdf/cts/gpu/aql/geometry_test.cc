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
#include "libamdf/cts/gpu/kernels/geometry_ids_gfx942.h"

namespace {

namespace kernel = kernels::gfx942_geometry_ids;

struct alignas(16) Arguments {
  // Global GPU address of the first seven-word record, after the prefix guard.
  uint64_t output;
  // Packet-matching XYZ sizes used to form global coordinates from raw IDs.
  std::array<uint32_t, 3> workgroup_size;
  // Packet-matching XY sizes used as output row pitch and plane height.
  std::array<uint32_t, 2> grid_size;
  // Changing token stored as the seventh word of every active record.
  uint32_t epoch;
};
static_assert(alignof(Arguments) == kernel::kKernargAlignment);
static_assert(sizeof(Arguments) == kernel::kKernargByteLength);
static_assert(sizeof(Arguments) == 32);
static_assert(offsetof(Arguments, output) == 0);
static_assert(offsetof(Arguments, workgroup_size) == 8);
static_assert(sizeof(Arguments::workgroup_size) == 12);
static_assert(offsetof(Arguments, grid_size) == 20);
static_assert(sizeof(Arguments::grid_size) == 8);
static_assert(offsetof(Arguments, epoch) == 28);

class AqlGeometryTest : public AqlDispatchTest {
 protected:
  void RunGeometry(std::array<aql::DispatchGeometry, 2> geometries);
};

void AqlGeometryTest::RunGeometry(
    std::array<aql::DispatchGeometry, 2> geometries) {
  constexpr uint32_t kRecordCapacity = 4096;
  constexpr uint32_t kRecordWordCount = 7;
  constexpr uint32_t kGuardWordCount = 16;
  constexpr uint32_t kWordCount =
      kRecordCapacity * kRecordWordCount + 2 * kGuardWordCount;
  constexpr uint32_t kPrefixGuard = 0x619b30d5u;
  constexpr uint32_t kSuffixGuard = 0xe270c84bu;
  constexpr uint32_t kInactiveWord = 0xb73a51c9u;
  constexpr std::array<uint32_t, 2> kEpochTokens = {0x13579bdfu, 0xa5c31f27u};
  constexpr std::array<const char*, 3> kAxes = {"x", "y", "z"};
  static_assert(kernel::kWorkgroupSize == 64);
  static_assert(kernel::kGroupSegmentByteLength == 0);
  static_assert(kernel::kPrivateSegmentByteLength == 0);

  amdf_gpu_endpoint_info_t endpoint_info = {};
  endpoint_info.type = AMDF_STRUCTURE_TYPE_GPU_ENDPOINT_INFO;
  endpoint_info.structure_size = sizeof(endpoint_info);
  ASSERT_EQ(gpu_api_->endpoint_query_info(endpoint_, &endpoint_info),
            AMDF_STATUS_OK);
  ASSERT_EQ(endpoint_info.compute.wavefront_size, 64u);
  std::array<uint32_t, 2> active_record_counts;
  for (uint32_t epoch = 0; epoch < geometries.size(); ++epoch) {
    const auto& geometry = geometries[epoch];
    ASSERT_EQ(uint64_t{geometry.workgroup_size[0]} *
                  geometry.workgroup_size[1] * geometry.workgroup_size[2],
              kernel::kWorkgroupSize);
    const uint64_t record_count = uint64_t{geometry.grid_size[0]} *
                                  geometry.grid_size[1] * geometry.grid_size[2];
    ASSERT_LE(record_count, kRecordCapacity);
    active_record_counts[epoch] = static_cast<uint32_t>(record_count);
  }
  RecordProperty("aql_geometry_record_capacity", kRecordCapacity);
  RecordProperty("aql_geometry_record_word_count", kRecordWordCount);
  RecordProperty("aql_geometry_checked_word_count", kWordCount);
  RecordProperty("aql_geometry_kernarg_byte_length", sizeof(Arguments));
  RecordProperty("aql_geometry_flat_workgroup_size", kernel::kWorkgroupSize);

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
  for (uint32_t epoch = 0; epoch < geometries.size(); ++epoch) {
    const auto& geometry = geometries[epoch];
    // The checked tail covers the selected two epochs' old/new
    // geometry-argument mixtures. Only matching packet and kernarg shapes are
    // submitted.
    expected.fill(kInactiveWord);
    for (uint32_t word = 0; word < kGuardWordCount; ++word) {
      expected[word] = kPrefixGuard;
      expected[kWordCount - kGuardWordCount + word] = kSuffixGuard;
    }
    uint32_t position = kGuardWordCount;
    // Walk global coordinates in storage order and recover each raw ID by
    // division/remainder, independently of the kernel's forward index formula.
    for (uint32_t z = 0; z < geometry.grid_size[2]; ++z) {
      for (uint32_t y = 0; y < geometry.grid_size[1]; ++y) {
        for (uint32_t x = 0; x < geometry.grid_size[0]; ++x) {
          expected[position++] = x / geometry.workgroup_size[0];
          expected[position++] = y / geometry.workgroup_size[1];
          expected[position++] = z / geometry.workgroup_size[2];
          expected[position++] = x % geometry.workgroup_size[0];
          expected[position++] = y % geometry.workgroup_size[1];
          expected[position++] = z % geometry.workgroup_size[2];
          expected[position++] = kEpochTokens[epoch];
        }
      }
    }
    observed = expected;
    for (uint32_t word = kGuardWordCount; word < position; ++word) {
      observed[word] = ~expected[word];
    }
    std::memcpy(output->host.pointer, observed.data(), sizeof(observed));
    const Arguments payload = {
        output->device_address + kGuardWordCount * sizeof(uint32_t),
        {geometry.workgroup_size[0], geometry.workgroup_size[1],
         geometry.workgroup_size[2]},
        {geometry.grid_size[0], geometry.grid_size[1]},
        kEpochTokens[epoch],
    };
    std::memcpy(arguments->host.pointer, &payload, sizeof(payload));
    ASSERT_EQ(
        GpuLoadAcquire<int64_t>(reinterpret_cast<uintptr_t>(&signal.value)), 0);
    signal.value = 1;
    const auto packet =
        aql::Dispatch(geometry, kernel::kPrivateSegmentByteLength,
                      kernel::kGroupSegmentByteLength, descriptor_address,
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
    const std::string prefix =
        "aql_geometry_epoch_" + std::to_string(epoch + 1);
    RecordProperty(prefix + "_token", std::to_string(kEpochTokens[epoch]));
    RecordProperty(prefix + "_dimensions", geometry.dimensions);
    RecordProperty(prefix + "_active_records", active_record_counts[epoch]);
    for (uint32_t axis = 0; axis < kAxes.size(); ++axis) {
      RecordProperty(prefix + "_workgroup_" + kAxes[axis],
                     geometry.workgroup_size[axis]);
      RecordProperty(prefix + "_grid_" + kAxes[axis], geometry.grid_size[axis]);
    }
  }
  RecordProperty("aql_geometry_completed_epochs", geometries.size());
}

TEST_F(AqlGeometryTest, TwoDimensionsRefreshWorkgroupAndGridShape) {
  constexpr std::array<aql::DispatchGeometry, 2> kGeometries = {{
      {2, {16, 4, 1}, {48, 8, 1}},
      {2, {4, 16, 1}, {8, 48, 1}},
  }};
  ASSERT_NO_FATAL_FAILURE(RunGeometry(kGeometries));
}

TEST_F(AqlGeometryTest, ThreeDimensionsRefreshWorkgroupAndGridShape) {
  constexpr std::array<aql::DispatchGeometry, 2> kGeometries = {{
      {3, {8, 4, 2}, {24, 8, 4}},
      {3, {4, 2, 8}, {8, 6, 16}},
  }};
  ASSERT_NO_FATAL_FAILURE(RunGeometry(kGeometries));
}

}  // namespace
