// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/util/fact_control.h"

#include <random>
#include <vector>

#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"
#include "loom/util/cfg_control_test_harness.h"
#include "loom/util/cfg_control_test_util.h"
#include "loom/util/cfg_graph_test_util.h"

namespace loom {
namespace {

using testing::ControlDistribution;

class FactControlTest : public ::testing::Test {
 protected:
  testing::CfgControlTestHarness harness_;
};

TEST_F(FactControlTest, NestedScopesStrengthenAndWeakenAcrossReconvergence) {
  std::mt19937 random(763234);
  IREE_ASSERT_OK(harness_.CheckFactUpdates(
      {{1, 6}, {2, 3}, {4}, {4}, {5}, {6}, {}}, random));
  IREE_ASSERT_OK(
      harness_.CheckFactUpdates({{1, 2}, {3}, {3}, {4, 5}, {}, {}}, random));
  IREE_ASSERT_OK(
      harness_.CheckFactUpdates({{3, 4}, {5}, {5}, {1, 2}, {5}, {}}, random));
}

TEST_F(FactControlTest, CyclesIrreducibilityAndUnreachableSelectors) {
  std::mt19937 random(54323);
  IREE_ASSERT_OK(harness_.CheckFactUpdates({{1}, {2, 3}, {1}, {}}, random));
  IREE_ASSERT_OK(
      harness_.CheckFactUpdates({{1, 2}, {2, 3}, {1, 3}, {}}, random));
  IREE_ASSERT_OK(harness_.CheckFactUpdates(
      {{1, 2}, {3, 4}, {4, 5}, {1}, {2, 5}, {}, {0, 6}}, random));
  IREE_ASSERT_OK(harness_.CheckFactUpdates({{1, 2}, {1}, {2}}, random));
}

TEST_F(FactControlTest, ExhaustiveThreeBlockGraphsWithNonmonotoneUpdates) {
  std::mt19937 random(738552);
  for (uint32_t mask = 0; mask < 512; ++mask) {
    SCOPED_TRACE(mask);
    std::vector<std::vector<uint16_t>> successors(3);
    for (uint16_t source = 0; source < 3; ++source) {
      for (uint16_t target = 0; target < 3; ++target) {
        if (mask & (1u << (source * 3 + target))) {
          successors[source].push_back(target);
        }
      }
    }
    IREE_ASSERT_OK(harness_.CheckFactUpdates(successors, random));
  }
}

TEST_F(FactControlTest, RandomGraphsAndMixedScopeUpdates) {
  std::mt19937 random(996137);
  // The manual stress test retains the full fixed-seed sequence, while
  // fact_control_fuzz owns unbounded topology and update-sequence search.
  for (uint32_t trial = 0; trial < 256; ++trial) {
    SCOPED_TRACE(trial);
    const uint32_t count = 1 + random() % 20;
    std::vector<std::vector<uint16_t>> successors(count);
    for (auto& edges : successors) {
      uint32_t edge_count = random() % 5;
      while (edge_count--) {
        edges.push_back(random() % count);
      }
    }
    IREE_ASSERT_OK(harness_.CheckFactUpdates(successors, random));
  }
}

TEST_F(FactControlTest, TransientResetDoesNotPublishAChange) {
  testing::CfgGraph fixture({{1, 2}, {3}, {3}, {}});
  loom_cfg_control_t structure;
  IREE_ASSERT_OK(
      loom_cfg_control_build(fixture.get(), harness_.arena(), &structure));
  loom_value_fact_control_t control;
  IREE_ASSERT_OK(loom_value_fact_control_initialize(
      &structure, harness_.arena(), &control));
  for (uint8_t initial = 0; initial < 5; ++initial) {
    loom_value_fact_control_set_selector(&control, 0,
                                         ControlDistribution(initial));
    loom_value_fact_control_settle(&control);
    uint16_t block = 0;
    while (loom_value_fact_control_take_changed_block(&control, &block)) {
    }
    loom_value_fact_control_set_selector(&control, 0, ControlDistribution(4));
    loom_value_fact_control_settle(&control);
    loom_value_fact_control_set_selector(&control, 0,
                                         ControlDistribution(initial));
    loom_value_fact_control_settle(&control);
    EXPECT_FALSE(loom_value_fact_control_take_changed_block(&control, &block));
  }
}

TEST_F(FactControlTest,
       EqualMinimumKeepsItsWitnessUntilThatSelectorStrengthens) {
  testing::CfgGraph fixture({{1, 4}, {2, 4}, {3, 4}, {4}, {}});
  loom_cfg_control_t structure;
  IREE_ASSERT_OK(
      loom_cfg_control_build(fixture.get(), harness_.arena(), &structure));
  loom_value_fact_control_t control;
  IREE_ASSERT_OK(loom_value_fact_control_initialize(
      &structure, harness_.arena(), &control));
  loom_value_fact_control_set_selector(&control, 0, ControlDistribution(1));
  loom_value_fact_control_settle(&control);
  loom_value_fact_control_prepare_diagnostics(&control);
  const auto original = loom_value_fact_control_controller(&control, 3);
  ASSERT_NE(original, LOOM_CFG_EDGE_INDEX_INVALID);
  EXPECT_EQ(fixture.get()->edges[original].source_block_index, 0);
  for (uint16_t block : {1, 2}) {
    loom_value_fact_control_set_selector(&control, block,
                                         ControlDistribution(1));
    loom_value_fact_control_settle(&control);
    loom_value_fact_control_prepare_diagnostics(&control);
    EXPECT_EQ(loom_value_fact_control_controller(&control, 3), original);
  }
  loom_value_fact_control_set_selector(&control, 0, ControlDistribution(4));
  loom_value_fact_control_settle(&control);
  loom_value_fact_control_prepare_diagnostics(&control);
  auto replacement = loom_value_fact_control_controller(&control, 3);
  ASSERT_NE(replacement, LOOM_CFG_EDGE_INDEX_INVALID);
  EXPECT_NE(fixture.get()->edges[replacement].source_block_index, 0);
  EXPECT_EQ(loom_value_fact_control_execution(&control, 3).flags,
            ControlDistribution(1).flags);
}

}  // namespace
}  // namespace loom
