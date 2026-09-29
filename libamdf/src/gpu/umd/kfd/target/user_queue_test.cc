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
  topology.properties.compute.maximum_scratch_wave_count_per_compute_unit = 32;
  topology.properties.compute.local_data_share_byte_length = 65536;
  topology.properties.topology.xcc_count = 1;
  topology.properties.topology.shader_engine_count_per_xcc = 1;
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
  ASSERT_EQ(plans.count, 3u);
  EXPECT_EQ(plans.values[0].family.command_type,
            AMDF_QUEUE_COMMAND_TYPE_GPU_PM4);
  EXPECT_EQ(plans.values[1].family.command_type,
            AMDF_QUEUE_COMMAND_TYPE_GPU_AQL);
  EXPECT_EQ(plans.values[2].family.command_type,
            AMDF_QUEUE_COMMAND_TYPE_GPU_SDMA);

  EXPECT_EQ(plans.values[2].family.format_features,
            AMDF_GPU_SDMA_FORMAT_FEATURE_FENCE_MEMORY_TYPE);
  EXPECT_EQ(plans.values[2].family.roles, AMDF_QUEUE_ROLE_TRANSFER);
  EXPECT_EQ(plans.values[2].family.cache_operations, 0u);
  EXPECT_EQ(plans.values[2].family.cache_transition_kinds, 0u);

  topology.compute_queue_count = 0;
  amdf_gpu_kfd_target_user_queue_plans_initialize(&topology, 4096, 64, &plans);
  ASSERT_EQ(plans.count, 1u);
  EXPECT_EQ(plans.values[0].family.command_type,
            AMDF_QUEUE_COMMAND_TYPE_GPU_SDMA);

  topology = MakeTopology();
  topology.sdma.engine_count = 0;
  amdf_gpu_kfd_target_user_queue_plans_initialize(&topology, 4096, 64, &plans);
  ASSERT_EQ(plans.count, 2u);
  EXPECT_EQ(plans.values[0].family.command_type,
            AMDF_QUEUE_COMMAND_TYPE_GPU_PM4);
  EXPECT_EQ(plans.values[1].family.command_type,
            AMDF_QUEUE_COMMAND_TYPE_GPU_AQL);

  topology.properties.gfx_ip.major = 0;
  amdf_gpu_kfd_target_user_queue_plans_initialize(&topology, 4096, 64, &plans);
  EXPECT_EQ(plans.count, 0u);
}

TEST(KfdTargetUserQueueTest, ExposesBothComputeLanguagesOnRdna) {
  for (uint32_t target :
       {110000u, 110001u, 110002u, 110003u, 110500u, 110501u, 110502u, 110503u,
        110700u, 110701u, 110702u, 120000u, 120001u, 120500u, 120501u}) {
    SCOPED_TRACE(target);
    auto topology = MakeTopology();
    topology.properties.gfx_ip = {target / 10000, (target / 100) % 100,
                                  target % 100};
    if (target >= 120500) {
      topology.gc_ip = {12, 1, 0, true};
    }
    amdf_gpu_kfd_user_queue_plans_t plans;
    amdf_gpu_kfd_target_user_queue_plans_initialize(&topology, 4096, 64,
                                                    &plans);
    ASSERT_EQ(plans.count, 3u);
    EXPECT_EQ(plans.values[0].family.command_type,
              AMDF_QUEUE_COMMAND_TYPE_GPU_PM4);
    EXPECT_EQ(plans.values[1].family.command_type,
              AMDF_QUEUE_COMMAND_TYPE_GPU_AQL);
    EXPECT_EQ(plans.values[1].family.format_features, 0u);
    EXPECT_EQ(plans.values[2].family.command_type,
              AMDF_QUEUE_COMMAND_TYPE_GPU_SDMA);
  }
}

TEST(KfdTargetUserQueueTest, CdnaDoesNotAdvertisePm4) {
  for (uint32_t target : {90400u, 90401u, 90402u, 90500u}) {
    SCOPED_TRACE(target);
    auto topology = MakeTopology();
    topology.properties.gfx_ip = {target / 10000, (target / 100) % 100,
                                  target % 100};
    topology.properties.compute.wavefront_size = 64;
    amdf_gpu_kfd_user_queue_plans_t plans;
    amdf_gpu_kfd_target_user_queue_plans_initialize(&topology, 4096, 64,
                                                    &plans);
    ASSERT_EQ(plans.count, 2u);
    EXPECT_EQ(plans.values[0].family.command_type,
              AMDF_QUEUE_COMMAND_TYPE_GPU_AQL);
    EXPECT_EQ(plans.values[0].family.format_features,
              AMDF_GPU_AQL_FORMAT_FEATURE_BARRIER_VALUE);
    EXPECT_EQ(plans.values[1].family.command_type,
              AMDF_QUEUE_COMMAND_TYPE_GPU_SDMA);
  }
}

TEST(KfdTargetUserQueueTest,
     RetainedScratchPadsBackingSeparatelyFromWaveLimit) {
  auto topology = MakeTopology();
  topology.properties.compute.compute_unit_count = 38;
  topology.properties.topology.shader_engine_count_per_xcc = 4;
  amdf_gpu_kfd_user_queue_plans_t plans;
  amdf_gpu_kfd_target_user_queue_plans_initialize(&topology, 4096, 64, &plans);
  ASSERT_EQ(plans.count, 3u);
  EXPECT_EQ(plans.values[1].aql.scratch.slot_count_per_xcc, 1280u);
  EXPECT_EQ(plans.values[1].aql.scratch.temporary_ring_wave_count, 320u);

  topology.properties.gfx_ip = {9, 4, 2};
  topology.properties.compute.wavefront_size = 64;
  amdf_gpu_kfd_target_user_queue_plans_initialize(&topology, 4096, 64, &plans);
  ASSERT_EQ(plans.count, 2u);
  EXPECT_EQ(plans.values[0].aql.scratch.slot_count_per_xcc, 1280u);
  EXPECT_EQ(plans.values[0].aql.scratch.temporary_ring_wave_count, 1216u);
}

TEST(KfdTargetUserQueueTest, ExtendedAperturesRequireNativeGcIdentity) {
  auto topology = MakeTopology();
  topology.properties.gfx_ip = {12, 5, 0};
  amdf_gpu_kfd_user_queue_plans_t plans;
  amdf_gpu_kfd_target_user_queue_plans_initialize(&topology, 4096, 64, &plans);
  ASSERT_EQ(plans.count, 2u);
  EXPECT_EQ(plans.values[0].family.command_type,
            AMDF_QUEUE_COMMAND_TYPE_GPU_PM4);
  EXPECT_EQ(plans.values[1].family.command_type,
            AMDF_QUEUE_COMMAND_TYPE_GPU_SDMA);
}

TEST(KfdTargetUserQueueTest, ExactSdma6HasClassicFenceWithoutCacheOperations) {
  amdf_gpu_kfd_topology_t topology = MakeTopology();
  topology.properties.gfx_ip = {};
  for (uint32_t minor : {0u, 1u}) {
    SCOPED_TRACE(minor);
    topology.sdma.ip = {6, minor, 0, true};
    amdf_gpu_kfd_user_queue_plans_t plans;
    amdf_gpu_kfd_target_user_queue_plans_initialize(&topology, 4096, 64,
                                                    &plans);
    ASSERT_EQ(plans.count, 1u);
    const auto& plan = plans.values[0];
    EXPECT_EQ(plan.family.command_type, AMDF_QUEUE_COMMAND_TYPE_GPU_SDMA);
    EXPECT_EQ(plan.family.format_version, AMDF_GPU_SDMA_QUEUE_FORMAT_VERSION_1);
    EXPECT_EQ(plan.family.format_features,
              AMDF_GPU_SDMA_FORMAT_FEATURE_FENCE_MEMORY_TYPE);
    EXPECT_EQ(plan.family.roles, AMDF_QUEUE_ROLE_TRANSFER);
    EXPECT_EQ(plan.family.cache_operations, 0u);
    EXPECT_EQ(plan.family.cache_transition_kinds, 0u);
    EXPECT_EQ(plan.family.publication_modes, AMDF_QUEUE_PUBLICATION_MODE_USER);
    EXPECT_EQ(plan.ring.primary_byte_length, 4096u);
    EXPECT_EQ(plan.control.index_bit_count, 64u);
    EXPECT_EQ(plan.control.read_index_mask, UINT64_MAX);
    EXPECT_EQ(plan.doorbell.bit_count, 64u);
    EXPECT_EQ(plan.compute.context_storage.byte_length, 0u);
  }
}

TEST(KfdTargetUserQueueTest,
     ExactSdma442HasTransferWithoutOptionalFenceFields) {
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
  ASSERT_EQ(plans.count, 2u);
  EXPECT_EQ(plans.values[0].family.command_type,
            AMDF_QUEUE_COMMAND_TYPE_GPU_PM4);
  EXPECT_EQ(plans.values[1].family.command_type,
            AMDF_QUEUE_COMMAND_TYPE_GPU_AQL);
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
  EXPECT_EQ(plan.aql.scratch.slot_count_per_xcc, 1280u);
  EXPECT_EQ(plan.aql.scratch.temporary_ring_wave_count, 1280u);
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

TEST(KfdTargetUserQueueTest, AqlTransferAdmissionKeepsEarlierTargetsUnchanged) {
  amdf_gpu_kfd_topology_t topology = MakeTopology();
  topology.properties.gfx_ip = {9, 4, 0};
  topology.properties.compute.wavefront_size = 64;
  topology.properties.compute.compute_unit_count = 304;
  topology.properties.compute.maximum_scratch_wave_count_per_compute_unit = 32;
  topology.properties.compute.local_data_share_byte_length = 65536;
  topology.properties.topology.xcc_count = 8;
  topology.properties.topology.shader_engine_count_per_xcc = 4;
  topology.context_save_restore_byte_length = 0;
  topology.control_stack_byte_length = 0;
  for (uint32_t stepping : {0u, 1u, 2u}) {
    SCOPED_TRACE(stepping);
    topology.properties.gfx_ip.stepping = stepping;
    amdf_gpu_kfd_user_queue_plans_t plans;
    amdf_gpu_kfd_target_user_queue_plans_initialize(&topology, 4096, 64,
                                                    &plans);
    ASSERT_EQ(plans.count, 2u);
    const auto& family = plans.values[0].family;
    EXPECT_EQ(family.command_type, AMDF_QUEUE_COMMAND_TYPE_GPU_AQL);
    EXPECT_EQ(family.format_version, AMDF_GPU_AQL_QUEUE_FORMAT_VERSION_1);
    EXPECT_EQ(family.format_features,
              AMDF_GPU_AQL_FORMAT_FEATURE_BARRIER_VALUE);
    const amdf_queue_roles_t expected_roles =
        AMDF_QUEUE_ROLE_COMPUTE | AMDF_QUEUE_ROLE_CACHE_CONTROL |
        (stepping == 2 ? AMDF_QUEUE_ROLE_TRANSFER : 0);
    EXPECT_EQ(family.roles, expected_roles);
    EXPECT_EQ(plans.values[0].compute.context_count, 8u);
    EXPECT_EQ(plans.values[1].family.command_type,
              AMDF_QUEUE_COMMAND_TYPE_GPU_SDMA);
  }
  topology.properties.gfx_ip.stepping = 3;
  amdf_gpu_kfd_user_queue_plans_t plans;
  amdf_gpu_kfd_target_user_queue_plans_initialize(&topology, 4096, 64, &plans);
  ASSERT_EQ(plans.count, 1u);
  EXPECT_EQ(plans.values[0].family.command_type,
            AMDF_QUEUE_COMMAND_TYPE_GPU_SDMA);
}

}  // namespace
