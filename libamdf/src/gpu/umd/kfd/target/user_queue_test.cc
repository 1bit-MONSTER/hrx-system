// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "libamdf/src/gpu/umd/kfd/target/user_queue.h"

#include "gtest/gtest.h"

namespace {

// Passive queue discovery has native topology but no activated address domain.
static amdf_gpu_kfd_topology_t MakeTopology() {
  amdf_gpu_kfd_topology_t topology = {};
  topology.properties.gfx_ip = {11, 5, 1};
  topology.properties.compute.wavefront_size = 32;
  topology.properties.compute.compute_unit_count = 2;
  topology.properties.compute.maximum_wave_count_per_compute_unit = 32;
  topology.properties.topology.xcc_count = 1;
  topology.compute_queue_count = 8;
  topology.sdma.engine_count = 1;
  topology.sdma.queue_count_per_engine = 6;
  topology.sdma.ip = {6, 1, 1, true};
  topology.context_save_restore_byte_length = 4096;
  topology.control_stack_byte_length = 4096;
  return topology;
}

TEST(KfdTargetUserQueueTest, KeepsSupportedPlansDense) {
  amdf_gpu_kfd_topology_t topology = MakeTopology();
  amdf_gpu_kfd_user_queue_plans_t plans;
  amdf_gpu_kfd_target_user_queue_plans_initialize(&topology, 4096, 64, &plans);
  ASSERT_EQ(plans.count, 2u);
  EXPECT_EQ(plans.values[0].family.command_type,
            AMDF_QUEUE_COMMAND_TYPE_GPU_PM4);
  EXPECT_EQ(plans.values[1].family.command_type,
            AMDF_QUEUE_COMMAND_TYPE_GPU_SDMA);
  EXPECT_EQ(plans.values[1].family.format_features,
            AMDF_GPU_SDMA_FORMAT_FEATURE_GCR |
                AMDF_GPU_SDMA_FORMAT_FEATURE_FENCE_MEMORY_TYPE);
  EXPECT_EQ(plans.values[1].family.roles,
            AMDF_QUEUE_ROLE_TRANSFER | AMDF_QUEUE_ROLE_CACHE_CONTROL);

  topology.compute_queue_count = 0;
  amdf_gpu_kfd_target_user_queue_plans_initialize(&topology, 4096, 64, &plans);
  ASSERT_EQ(plans.count, 1u);
  EXPECT_EQ(plans.values[0].family.command_type,
            AMDF_QUEUE_COMMAND_TYPE_GPU_SDMA);

  topology = MakeTopology();
  topology.sdma.engine_count = 0;
  amdf_gpu_kfd_target_user_queue_plans_initialize(&topology, 4096, 64, &plans);
  ASSERT_EQ(plans.count, 1u);
  EXPECT_EQ(plans.values[0].family.command_type,
            AMDF_QUEUE_COMMAND_TYPE_GPU_PM4);

  topology.properties.gfx_ip.major = 0;
  amdf_gpu_kfd_target_user_queue_plans_initialize(&topology, 4096, 64, &plans);
  EXPECT_EQ(plans.count, 0u);
}

TEST(KfdTargetUserQueueTest, ExactSdma442HasTransferWithoutGcrOrFenceFields) {
  amdf_gpu_kfd_topology_t topology = MakeTopology();
  // SDMA admission consumes its own exact IP, independent of compute layout.
  topology.properties.gfx_ip = {};
  topology.sdma.ip = {4, 4, 2, true};
  topology.sdma.engine_count = 2;
  topology.sdma.queue_count_per_engine = 8;
  amdf_gpu_kfd_user_queue_plans_t plans;
  amdf_gpu_kfd_target_user_queue_plans_initialize(&topology, 4096, 64, &plans);
  ASSERT_EQ(plans.count, 1u);
  const auto& plan = plans.values[0];
  EXPECT_EQ(plan.family.command_type, AMDF_QUEUE_COMMAND_TYPE_GPU_SDMA);
  EXPECT_EQ(plan.family.format_version, AMDF_GPU_SDMA_QUEUE_FORMAT_VERSION_1);
  EXPECT_EQ(plan.family.format_features, 0u);
  EXPECT_EQ(plan.family.roles, AMDF_QUEUE_ROLE_TRANSFER);
  EXPECT_EQ(plan.family.cache_operations, 0u);
  EXPECT_EQ(plan.family.cache_transition_kinds, 0u);
  EXPECT_EQ(plan.ring.primary_byte_length, 4096u);
  EXPECT_EQ(plan.control.index_bit_count, 64u);
  EXPECT_EQ(plan.control.read_index_mask, UINT64_MAX);
  EXPECT_EQ(plan.doorbell.bit_count, 64u);
  EXPECT_EQ(plan.compute.context_storage.byte_length, 0u);

  topology.sdma.ip.exact = false;
  amdf_gpu_kfd_target_user_queue_plans_initialize(&topology, 4096, 64, &plans);
  EXPECT_EQ(plans.count, 0u);
  topology.sdma.ip.exact = true;
  for (uint32_t revision : {0u, 1u, 3u}) {
    topology.sdma.ip.revision = revision;
    amdf_gpu_kfd_target_user_queue_plans_initialize(&topology, 4096, 64,
                                                    &plans);
    EXPECT_EQ(plans.count, 0u) << revision;
  }
  topology.sdma.ip = {4, 3, 2, true};
  amdf_gpu_kfd_target_user_queue_plans_initialize(&topology, 4096, 64, &plans);
  EXPECT_EQ(plans.count, 0u);
  topology.sdma.ip = {6, 2, 0, true};
  amdf_gpu_kfd_target_user_queue_plans_initialize(&topology, 4096, 64, &plans);
  EXPECT_EQ(plans.count, 0u);
}

TEST(KfdTargetUserQueueTest, ComputeAndDmaUseIndependentEngineRequirements) {
  amdf_gpu_kfd_topology_t topology = MakeTopology();
  // A compute layout this implementation cannot initialize must not suppress
  // a supported SDMA engine, which has no compute CWSR storage.
  topology.properties.gfx_ip = {};
  topology.properties.topology.xcc_count = 8;
  topology.context_save_restore_byte_length = 0;
  topology.control_stack_byte_length = 0;
  amdf_gpu_kfd_user_queue_plans_t plans;
  amdf_gpu_kfd_target_user_queue_plans_initialize(&topology, 4096, 64, &plans);
  ASSERT_EQ(plans.count, 1u);
  EXPECT_EQ(plans.values[0].family.command_type,
            AMDF_QUEUE_COMMAND_TYPE_GPU_SDMA);
  EXPECT_EQ(plans.values[0].compute.context_storage.byte_length, 0u);

  // Conversely, lack of a native SDMA IP query does not invalidate compute.
  topology = MakeTopology();
  topology.sdma.ip.exact = false;
  amdf_gpu_kfd_target_user_queue_plans_initialize(&topology, 4096, 64, &plans);
  ASSERT_EQ(plans.count, 1u);
  EXPECT_EQ(plans.values[0].family.command_type,
            AMDF_QUEUE_COMMAND_TYPE_GPU_PM4);
}

TEST(KfdTargetUserQueueTest,
     RejectsUnrepresentableWaveStorageWithoutLosingDma) {
  amdf_gpu_kfd_topology_t topology = MakeTopology();
  topology.properties.compute.compute_unit_count = UINT32_MAX;
  topology.properties.compute.maximum_wave_count_per_compute_unit = UINT32_MAX;
  amdf_gpu_kfd_user_queue_plans_t plans;
  amdf_gpu_kfd_target_user_queue_plans_initialize(&topology, 4096, 64, &plans);
  ASSERT_EQ(plans.count, 1u);
  EXPECT_EQ(plans.values[0].family.command_type,
            AMDF_QUEUE_COMMAND_TYPE_GPU_SDMA);
}

TEST(KfdTargetUserQueueTest, ResolvesMultiXccSaveLayoutOnOlderKernels) {
  amdf_gpu_kfd_topology_t topology = MakeTopology();
  topology.properties.gfx_ip = {9, 4, 2};
  topology.properties.compute.wavefront_size = 64;
  topology.properties.compute.compute_unit_count = 304;
  topology.properties.compute.maximum_wave_count_per_compute_unit = 32;
  topology.properties.compute.maximum_scratch_wave_count_per_compute_unit = 32;
  topology.properties.compute.local_data_share_byte_length = 65536;
  topology.properties.topology.xcc_count = 8;
  topology.properties.topology.shader_engine_count_per_xcc = 4;
  topology.context_save_restore_byte_length = 0;
  topology.control_stack_byte_length = 0;
  amdf_gpu_kfd_user_queue_plans_t plans;
  amdf_gpu_kfd_target_user_queue_plans_initialize(&topology, 4096, 64, &plans);
  ASSERT_EQ(plans.count, 2u);
  const auto& plan = plans.values[0];
  EXPECT_EQ(plan.family.command_type, AMDF_QUEUE_COMMAND_TYPE_GPU_AQL);
  EXPECT_EQ(plan.compute.context_count, 8u);
  EXPECT_EQ(plan.compute.control_stack_byte_length, 12288u);
  EXPECT_EQ(plan.compute.context_save_restore_byte_length, 23203840u);
  EXPECT_EQ(plan.compute.debug_byte_offset, 8u * 23203840);
  EXPECT_EQ(plan.compute.debug_byte_length, 389120u);
  EXPECT_EQ(plan.aql.scratch_wave_count_per_xcc, 1216u);
  EXPECT_EQ(plan.compute.end_of_pipe_storage.byte_length, 0u);

  // Explicit native geometry replaces the architectural size calculation.
  topology.context_save_restore_byte_length = 24576000;
  topology.control_stack_byte_length = 16384;
  amdf_gpu_kfd_target_user_queue_plans_initialize(&topology, 4096, 64, &plans);
  ASSERT_EQ(plans.count, 2u);
  EXPECT_EQ(plans.values[0].compute.context_save_restore_byte_length,
            24576000u);
  EXPECT_EQ(plans.values[0].compute.control_stack_byte_length, 16384u);

  topology.properties.compute.compute_unit_count = 303;
  amdf_gpu_kfd_target_user_queue_plans_initialize(&topology, 4096, 64, &plans);
  ASSERT_EQ(plans.count, 1u);
  EXPECT_EQ(plans.values[0].family.command_type,
            AMDF_QUEUE_COMMAND_TYPE_GPU_SDMA);
}

}  // namespace
