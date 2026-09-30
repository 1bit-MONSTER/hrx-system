// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "libamdf/src/gpu/umd/kfd/target/compute_storage.h"

#include <cstring>

#include "gtest/gtest.h"

namespace {

static amdf_gpu_kfd_topology_t MakeTopology(uint32_t major, uint32_t minor,
                                            uint32_t stepping) {
  amdf_gpu_kfd_topology_t topology = {};
  topology.properties.gfx_ip = {major, minor, stepping};
  topology.properties.compute.compute_unit_count = 40;
  topology.properties.compute.maximum_wave_count_per_compute_unit = 32;
  topology.properties.compute.local_data_share_byte_length = 65536;
  topology.properties.topology.xcc_count = 1;
  topology.properties.topology.shader_engine_count_per_xcc = 2;
  topology.context_save_restore_byte_length = 19013632;
  topology.control_stack_byte_length = 16384;
  return topology;
}

TEST(KfdComputeStorageTest, RdnaUsesNativeSaveSizesForBothPacketLanguages) {
  // The kernel supplies ASIC-dependent vector-register storage sizes. The
  // userspace debug tail follows the save ABI, not reported resident waves.
  for (uint32_t minor : {0u, 5u, 7u}) {
    SCOPED_TRACE(minor);
    auto topology = MakeTopology(11, minor, 0);
    topology.properties.compute.maximum_wave_count_per_compute_unit = 16;
    for (auto command_type :
         {AMDF_QUEUE_COMMAND_TYPE_GPU_PM4, AMDF_QUEUE_COMMAND_TYPE_GPU_AQL}) {
      SCOPED_TRACE(command_type);
      amdf_gpu_kfd_compute_storage_plan_t plan;
      ASSERT_TRUE(amdf_gpu_kfd_compute_storage_plan(&topology, command_type,
                                                    4096, &plan));
      EXPECT_EQ(plan.context_count, 1u);
      EXPECT_EQ(plan.context_save_restore_byte_length, 19013632u);
      EXPECT_EQ(plan.control_stack_byte_length, 16384u);
      EXPECT_EQ(plan.debug_byte_offset, 19013632u);
      EXPECT_EQ(plan.debug_byte_length, 40960u);
      EXPECT_EQ(plan.context_storage.byte_length, 19054592u);
      // Writable, executable, coherent GTT and writable, executable VRAM.
      EXPECT_EQ(plan.context_storage.native_flags, UINT32_C(0xc4000002));
      EXPECT_EQ(plan.end_of_pipe_storage.native_flags, UINT32_C(0xc0000001));
      EXPECT_EQ(plan.context_storage.host_access,
                AMDF_GPU_KFD_BUFFER_HOST_ACCESS_MAPPED);
      EXPECT_EQ(plan.end_of_pipe_storage.byte_length, 4096u);
      EXPECT_EQ(plan.end_of_pipe_storage.host_access,
                AMDF_GPU_KFD_BUFFER_HOST_ACCESS_NONE);
    }
  }
}

TEST(KfdComputeStorageTest, Gfx12SaveProtocolsKeepEveryXccContext) {
  for (uint32_t minor : {0u, 5u}) {
    SCOPED_TRACE(minor);
    auto topology = MakeTopology(12, minor, 0);
    topology.properties.compute.compute_unit_count = 64;
    topology.properties.topology.xcc_count = 2;
    topology.context_save_restore_byte_length = 0x100000;
    for (auto command_type :
         {AMDF_QUEUE_COMMAND_TYPE_GPU_PM4, AMDF_QUEUE_COMMAND_TYPE_GPU_AQL}) {
      SCOPED_TRACE(command_type);
      amdf_gpu_kfd_compute_storage_plan_t plan;
      ASSERT_TRUE(amdf_gpu_kfd_compute_storage_plan(&topology, command_type,
                                                    4096, &plan));
      EXPECT_EQ(plan.context_count, 2u);
      EXPECT_EQ(plan.context_save_restore_byte_length, 0x100000u);
      EXPECT_EQ(plan.control_stack_byte_length, 16384u);
      EXPECT_EQ(plan.debug_byte_offset, 0x200000u);
      EXPECT_EQ(plan.debug_byte_length, minor == 5 ? 131072u : 65536u);
      EXPECT_EQ(plan.context_storage.byte_length,
                minor == 5 ? 2228224u : 2162688u);
      EXPECT_EQ(plan.end_of_pipe_storage.byte_length, 4096u);
    }
  }
}

TEST(KfdComputeStorageTest, CdnaUsesNodeShaderEngineBoundForPerXccDebugArea) {
  auto topology = MakeTopology(9, 4, 2);
  topology.properties.compute.compute_unit_count = 80;
  topology.properties.topology.xcc_count = 2;
  topology.properties.topology.shader_engine_count_per_xcc = 1;
  amdf_gpu_kfd_compute_storage_plan_t plan;
  ASSERT_TRUE(amdf_gpu_kfd_compute_storage_plan(
      &topology, AMDF_QUEUE_COMMAND_TYPE_GPU_AQL, 4096, &plan));
  // min(40 CUs * 40 saved waves, 2 node shader engines * 512 waves).
  EXPECT_EQ(plan.debug_byte_length, 65536u);
  EXPECT_EQ(plan.context_count, 2u);
  EXPECT_EQ(plan.debug_byte_offset, 38027264u);
  EXPECT_EQ(plan.end_of_pipe_storage.byte_length, 0u);
}

TEST(KfdComputeStorageTest, Cdna4FallbackSavesExpandedLdsAndAllocatesEop) {
  auto topology = MakeTopology(9, 5, 0);
  topology.properties.compute.compute_unit_count = 8;
  topology.properties.compute.local_data_share_byte_length = 163840;
  topology.properties.topology.xcc_count = 2;
  topology.context_save_restore_byte_length = 0;
  topology.control_stack_byte_length = 0;
  amdf_gpu_kfd_compute_storage_plan_t plan;
  ASSERT_TRUE(amdf_gpu_kfd_compute_storage_plan(
      &topology, AMDF_QUEUE_COMMAND_TYPE_GPU_AQL, 4096, &plan));
  EXPECT_EQ(plan.control_stack_byte_length, 4096u);
  EXPECT_EQ(plan.context_save_restore_byte_length, 2838528u);
  EXPECT_EQ(plan.debug_byte_length, 10240u);
  EXPECT_EQ(plan.context_storage.byte_length, 5689344u);
  EXPECT_EQ(plan.end_of_pipe_storage.byte_length, 4096u);
}

TEST(KfdComputeStorageTest, IncompleteOrUnrepresentableStorageNeverPublishes) {
  for (uint32_t scenario = 0; scenario < 9; ++scenario) {
    SCOPED_TRACE(scenario);
    auto topology = MakeTopology(11, 5, 1);
    size_t page_size = 4096;
    switch (scenario) {
      case 0:
        topology.context_save_restore_byte_length = 0;
        topology.control_stack_byte_length = 0;
        break;
      case 1:
        topology.control_stack_byte_length = 0;
        break;
      case 2:
        ++topology.context_save_restore_byte_length;
        break;
      case 3:
        topology.context_save_restore_byte_length = 4096;
        break;
      case 4:
        topology.properties.topology.xcc_count = 3;
        break;
      case 5:
        topology.properties.compute.compute_unit_count = UINT32_MAX;
        break;
      case 6:
        topology.context_save_restore_byte_length = 0xfffff000;
        topology.properties.topology.xcc_count = 2;
        break;
      case 7:
        topology.properties.gfx_ip = {12, 9, 0};
        break;
      case 8:
        page_size = 65536;
        break;
    }
    amdf_gpu_kfd_compute_storage_plan_t plan;
    std::memset(&plan, 0xA5, sizeof(plan));
    const auto original = plan;
    EXPECT_FALSE(amdf_gpu_kfd_compute_storage_plan(
        &topology, AMDF_QUEUE_COMMAND_TYPE_GPU_AQL, page_size, &plan));
    EXPECT_EQ(std::memcmp(&plan, &original, sizeof(plan)), 0);
  }
}

}  // namespace
