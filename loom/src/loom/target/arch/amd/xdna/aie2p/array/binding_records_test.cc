// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/target/arch/amd/xdna/aie2p/array/binding_records.h"

#include "iree/testing/gtest.h"

namespace loom {
namespace {

TEST(Aie2pArrayBindingRecordsTest, PreservesSparseAbiAndMaximumSpan) {
  const loom_aie2p_array_binding_t bindings[] = {
      {
          /*.value_id=*/0,
          /*.ordinal=*/0,
          /*.access=*/LOOM_AIE2P_ARRAY_BINDING_ACCESS_READ,
      },
      {
          /*.value_id=*/1,
          /*.ordinal=*/2,
          /*.access=*/LOOM_AIE2P_ARRAY_BINDING_ACCESS_WRITE,
      },
  };
  const loom_aie2p_array_binding_plan_t binding_plans[] = {
      {
          /*.binding_index=*/0,
          /*.channel_index=*/0,
          /*.dma_index=*/0,
          /*.partition_lane=*/0,
          /*.partition_lane_count=*/1,
          /*.completion_route_index=*/UINT32_MAX,
          /*.binding_byte_offset=*/64,
          /*.binding_span_byte_length=*/32,
      },
      {
          /*.binding_index=*/0,
          /*.channel_index=*/1,
          /*.dma_index=*/1,
          /*.partition_lane=*/0,
          /*.partition_lane_count=*/1,
          /*.completion_route_index=*/UINT32_MAX,
          /*.binding_byte_offset=*/16,
          /*.binding_span_byte_length=*/256,
      },
      {
          /*.binding_index=*/1,
          /*.channel_index=*/2,
          /*.dma_index=*/2,
          /*.partition_lane=*/0,
          /*.partition_lane_count=*/1,
          /*.completion_route_index=*/UINT32_MAX,
          /*.binding_byte_offset=*/128,
          /*.binding_span_byte_length=*/64,
      },
  };
  loom_aie2p_array_plan_t plan = {};
  plan.family = loom_xdna_npu2_array_family();
  plan.bindings = bindings;
  plan.binding_count = IREE_ARRAYSIZE(bindings);
  plan.binding_slot_count = 3;
  plan.binding_plans = binding_plans;
  plan.binding_plan_count = IREE_ARRAYSIZE(binding_plans);

  iree_xdna_elf_binding_record_t records[3];
  loom_aie2p_array_binding_records_build(&plan, records);

  EXPECT_EQ(records[0].kind, IREE_XDNA_ELF_BINDING_KIND_BUFFER);
  EXPECT_EQ(records[0].access, IREE_XDNA_ELF_BINDING_ACCESS_READ);
  EXPECT_EQ(records[0].minimum_byte_length, 272u);
  EXPECT_GT(records[0].minimum_alignment, 1u);
  EXPECT_EQ(records[1].kind, 0u);
  EXPECT_EQ(records[1].minimum_byte_length, 0u);
  EXPECT_EQ(records[2].kind, IREE_XDNA_ELF_BINDING_KIND_BUFFER);
  EXPECT_EQ(records[2].access, IREE_XDNA_ELF_BINDING_ACCESS_WRITE);
  EXPECT_EQ(records[2].minimum_byte_length, 192u);
  EXPECT_EQ(records[2].minimum_alignment, records[0].minimum_alignment);
}

TEST(Aie2pArrayBindingRecordsTest, AcceptsEmptyAbi) {
  const loom_aie2p_array_plan_t plan = {};
  loom_aie2p_array_binding_records_build(&plan, nullptr);
}

}  // namespace
}  // namespace loom
