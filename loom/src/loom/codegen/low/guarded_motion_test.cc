// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/codegen/low/guarded_motion.h"

#include "iree/testing/gtest.h"

namespace loom {
namespace {

class GuardedMotionProfitabilityTest : public ::testing::Test {
 protected:
  void SetUp() override {
    nodes_[0].block_index = 0;
    nodes_[1].block_index = 1;
    blocks_[0].issue_group_start = 0;
    blocks_[0].issue_group_count = 1;
    blocks_[1].issue_group_start = 1;
    blocks_[1].issue_group_count = 1;
    baseline_ = {};
    baseline_.nodes = nodes_;
    baseline_.blocks = blocks_;
    baseline_.block_count = IREE_ARRAYSIZE(blocks_);
    baseline_.issue_groups = baseline_groups_;
    trial_ = baseline_;
    trial_.issue_groups = trial_groups_;
    region_.node_start = 1;
    plan_.regions = &region_;
    plan_.region_count = 1;
  }

  loom_low_schedule_node_t nodes_[2] = {};
  loom_low_schedule_block_t blocks_[2] = {};
  loom_low_schedule_issue_group_t baseline_groups_[2] = {};
  loom_low_schedule_issue_group_t trial_groups_[2] = {};
  loom_low_schedule_table_t baseline_ = {};
  loom_low_schedule_table_t trial_ = {};
  loom_low_guarded_motion_region_t region_ = {};
  loom_low_guarded_motion_plan_t plan_ = {};
};

TEST_F(GuardedMotionProfitabilityTest, RequiresGuardedSourceImprovement) {
  baseline_groups_[0].issue_cycle = 10;
  baseline_groups_[1].issue_cycle = 5;
  trial_groups_[0].issue_cycle = 9;
  trial_groups_[1].issue_cycle = 5;

  EXPECT_FALSE(
      loom_low_guarded_motion_improves_schedule(&plan_, &baseline_, &trial_));
}

TEST_F(GuardedMotionProfitabilityTest, AcceptsGuardedSourceImprovement) {
  baseline_groups_[0].issue_cycle = 10;
  baseline_groups_[1].issue_cycle = 5;
  trial_groups_[0].issue_cycle = 10;
  trial_groups_[1].issue_cycle = 4;

  EXPECT_TRUE(
      loom_low_guarded_motion_improves_schedule(&plan_, &baseline_, &trial_));
}

TEST_F(GuardedMotionProfitabilityTest, RejectsAnyBlockRegression) {
  baseline_groups_[0].issue_cycle = 10;
  baseline_groups_[1].issue_cycle = 5;
  trial_groups_[0].issue_cycle = 11;
  trial_groups_[1].issue_cycle = 4;

  EXPECT_FALSE(
      loom_low_guarded_motion_improves_schedule(&plan_, &baseline_, &trial_));
}

}  // namespace
}  // namespace loom
