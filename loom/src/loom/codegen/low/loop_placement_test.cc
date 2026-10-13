// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/codegen/low/loop_placement.h"

#include "iree/testing/gtest.h"

namespace loom {
namespace {

class LoopPlacementEconomicsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    block_.region_index = 0;
    copy_op_.parent_block = &block_;
    copy_op_.block_ordinal = 0;
    descriptor_op_.parent_block = &block_;
    descriptor_op_.block_ordinal = 1;
    nodes_[0].op = &copy_op_;
    nodes_[0].block_index = 0;
    nodes_[0].result_count = 1;
    nodes_[0].value_ordinals.inline_value_ordinals[0] = 0;
    nodes_[1].op = &descriptor_op_;
    nodes_[1].block_index = 0;
    nodes_[1].result_count = 1;
    nodes_[1].value_ordinals.inline_value_ordinals[0] = 1;
    schedule_block_.block = &block_;
    schedule_block_.node_count = IREE_ARRAYSIZE(nodes_);
    schedule_.nodes = nodes_;
    schedule_.node_count = IREE_ARRAYSIZE(nodes_);
    schedule_.blocks = &schedule_block_;
    schedule_.block_count = 1;

    baseline_spills_[0] = {.value_id = 42, .store_count = 1, .reload_count = 2};
    baseline_spills_[1] = {.value_id = 43, .store_count = 1, .reload_count = 1};
    baseline_.spill_plans = baseline_spills_;
    baseline_.spill_plan_count = IREE_ARRAYSIZE(baseline_spills_);
    baseline_.spill_count = IREE_ARRAYSIZE(baseline_spills_);
    baseline_.packet_move_count = 1;

    trial_spill_ = {.value_id = 43, .store_count = 1, .reload_count = 1};
    assignments_[0] = {
        .value_id = 41,
        .location_kind = LOOM_LOW_ALLOCATION_LOCATION_TARGET_ID,
        .location_count = 1,
    };
    assignments_[1] = {
        .value_id = 42,
        .location_kind = LOOM_LOW_ALLOCATION_LOCATION_TARGET_ID,
        .location_count = 1,
    };
    value_ids_[0] = 41;
    value_ids_[1] = 42;
    assignment_indices_[0] = 0;
    assignment_indices_[1] = 1;
    trial_.liveness.value_ids = value_ids_;
    trial_.liveness.value_count = IREE_ARRAYSIZE(value_ids_);
    trial_.assignments = assignments_;
    trial_.assignment_count = IREE_ARRAYSIZE(assignments_);
    trial_.assignment_indices_by_value_ordinal = assignment_indices_;
    trial_.spill_plans = &trial_spill_;
    trial_.spill_plan_count = 1;
    trial_.spill_count = 1;
    trial_.packet_move_count = 1;

    candidate_.copy_op = &copy_op_;
    candidate_.descriptor_op = &descriptor_op_;
    plan_.candidates = &candidate_;
    plan_.candidate_count = 1;
    plan_.baseline_spill_value_ids = baseline_spill_value_ids_;
    plan_.baseline_spill_value_count =
        IREE_ARRAYSIZE(baseline_spill_value_ids_);
  }

  loom_block_t block_ = {};
  loom_op_t copy_op_ = {};
  loom_op_t descriptor_op_ = {};
  loom_low_schedule_node_t nodes_[2] = {};
  loom_low_schedule_block_t schedule_block_ = {};
  loom_low_schedule_table_t schedule_ = {};
  loom_low_allocation_spill_plan_t baseline_spills_[2] = {};
  loom_low_allocation_table_t baseline_ = {};
  loom_low_allocation_spill_plan_t trial_spill_ = {};
  loom_low_allocation_assignment_t assignments_[2] = {};
  loom_value_id_t value_ids_[2] = {};
  uint32_t assignment_indices_[2] = {};
  loom_low_allocation_table_t trial_ = {};
  loom_low_loop_placement_candidate_t candidate_ = {};
  loom_value_id_t baseline_spill_value_ids_[2] = {42, 43};
  loom_low_loop_placement_plan_t plan_ = {};
};

TEST_F(LoopPlacementEconomicsTest, RequiresCompleteAllocationImprovement) {
  EXPECT_TRUE(loom_low_loop_placement_improves_allocation(&plan_, &baseline_,
                                                          &schedule_, &trial_));

  trial_.spill_plan_count = baseline_.spill_plan_count;
  trial_.spill_count = baseline_.spill_count;
  EXPECT_FALSE(loom_low_loop_placement_improves_allocation(
      &plan_, &baseline_, &schedule_, &trial_));
  trial_.spill_plan_count = 1;
  trial_.spill_count = 1;

  trial_.packet_move_count = 2;
  EXPECT_FALSE(loom_low_loop_placement_improves_allocation(
      &plan_, &baseline_, &schedule_, &trial_));
  trial_.packet_move_count = 1;

  trial_spill_.value_id = 44;
  EXPECT_FALSE(loom_low_loop_placement_improves_allocation(
      &plan_, &baseline_, &schedule_, &trial_));
  trial_spill_.value_id = 43;

  trial_spill_.store_count = 3;
  EXPECT_FALSE(loom_low_loop_placement_improves_allocation(
      &plan_, &baseline_, &schedule_, &trial_));
  trial_spill_.store_count = 1;

  assignments_[1].location_kind = LOOM_LOW_ALLOCATION_LOCATION_SPILL_SLOT;
  EXPECT_FALSE(loom_low_loop_placement_improves_allocation(
      &plan_, &baseline_, &schedule_, &trial_));
  assignments_[1].location_kind = LOOM_LOW_ALLOCATION_LOCATION_TARGET_ID;

  assignments_[0].location_kind = LOOM_LOW_ALLOCATION_LOCATION_SPILL_SLOT;
  EXPECT_FALSE(loom_low_loop_placement_improves_allocation(
      &plan_, &baseline_, &schedule_, &trial_));
}

TEST(LoopPlacementScheduleTest, BoundsPositiveGrowthByMovedChainCost) {
  loom_block_t block = {.region_index = 0};
  loom_op_t copy_op = {.block_ordinal = 1, .parent_block = &block};
  loom_low_schedule_block_t blocks[2] = {};
  blocks[0].block = &block;
  blocks[0].node_count = 1;
  blocks[0].issue_group_count = 1;
  blocks[1].issue_group_start = 1;
  blocks[1].issue_group_count = 1;
  loom_low_schedule_issue_group_t baseline_groups[2] = {};
  baseline_groups[0].issue_cycle = 10;
  baseline_groups[1].issue_cycle = 4;
  loom_low_schedule_issue_group_t trial_groups[2] = {};
  trial_groups[0].issue_cycle = 7;
  trial_groups[1].issue_cycle = 9;
  loom_low_schedule_table_t baseline = {
      .blocks = blocks,
      .block_count = IREE_ARRAYSIZE(blocks),
      .issue_groups = baseline_groups,
  };
  loom_low_schedule_table_t trial = {
      .blocks = blocks,
      .block_count = IREE_ARRAYSIZE(blocks),
      .issue_groups = trial_groups,
  };
  loom_low_loop_placement_candidate_t candidate = {
      .copy_op = &copy_op,
      .maximum_schedule_cycle_count = 5,
  };
  uint32_t schedule_cycle_allowances[2] = {0, 5};
  loom_low_loop_placement_plan_t plan = {
      .candidates = &candidate,
      .candidate_count = 1,
      .schedule_cycle_allowances_by_block = schedule_cycle_allowances,
  };

  EXPECT_TRUE(
      loom_low_loop_placement_bounds_schedule(&plan, &baseline, &trial));
  trial_groups[0].issue_cycle = 11;
  trial_groups[1].issue_cycle = 8;
  EXPECT_FALSE(
      loom_low_loop_placement_bounds_schedule(&plan, &baseline, &trial));
  trial_groups[0].issue_cycle = 7;
  trial_groups[1].issue_cycle = 10;
  EXPECT_FALSE(
      loom_low_loop_placement_bounds_schedule(&plan, &baseline, &trial));
}

}  // namespace
}  // namespace loom
