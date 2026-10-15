// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <random>
#include <vector>

#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"
#include "loom/util/cfg_graph_test_util.h"
#include "loom/util/cfg_regions_test_util.h"

namespace loom {
namespace {

using testing::CfgGraph;

class CfgRegionsTest : public ::testing::Test {
 protected:
  testing::CfgRegionsTestHarness harness_;
};

TEST_F(CfgRegionsTest, EmptyUnreachableAndReturningRegions) {
  IREE_ASSERT_OK(harness_.Check({}));
  IREE_ASSERT_OK(harness_.Check({{}}));
  IREE_ASSERT_OK(harness_.Check({{1, 2}, {}, {}}));
  IREE_ASSERT_OK(harness_.Check({{1}, {}, {1}}));
  IREE_ASSERT_OK(harness_.Check({{0}}));
}

TEST_F(CfgRegionsTest, SharedTailsAndParallelExits) {
  IREE_ASSERT_OK(harness_.Check({{1, 2}, {3, 2}, {4}, {4}, {}}));
  IREE_ASSERT_OK(harness_.Check({{1, 2}, {3}, {3, 4}, {4}, {}}));
  IREE_ASSERT_OK(harness_.Check({{1, 2}, {3, 3}, {3}, {}}));
  IREE_ASSERT_OK(harness_.Check({{4, 1}, {2}, {3}, {}, {2}}));
}

TEST_F(CfgRegionsTest, NestedLoopsAndIrreducibleControl) {
  IREE_ASSERT_OK(harness_.Check({{1, 6}, {2, 5}, {3, 4}, {2}, {1}, {6}, {}}));
  IREE_ASSERT_OK(harness_.Check({{1, 2}, {2, 3}, {1, 3}, {}}));
  IREE_ASSERT_OK(harness_.Check({{1, 2}, {1}, {3}, {}}));
}

TEST_F(CfgRegionsTest, DeterministicArbitraryGraphs) {
  std::mt19937 random(634);
  // The manual stress test retains the full fixed-seed sequence and exhaustive
  // four-block search; presubmit samples larger graphs from the same sequence.
  for (size_t sample = 0; sample < 256; ++sample) {
    SCOPED_TRACE(sample);
    uint16_t count = 2 + random() % 31;
    std::vector<std::vector<uint16_t>> successors(count);
    for (auto& outgoing : successors) {
      size_t edge_count = random() % 5;
      for (size_t i = 0; i < edge_count; ++i) {
        outgoing.push_back(random() % count);
      }
    }
    IREE_ASSERT_OK(harness_.Check(successors));
  }
}

TEST_F(CfgRegionsTest, DeepDominanceWithoutInclusiveRegionStorage) {
  constexpr uint16_t count = 12001;
  std::vector<std::vector<uint16_t>> successors(count);
  for (uint16_t block = 0; block + 3 < count; block += 3) {
    successors[block] = {uint16_t(block + 1), uint16_t(block + 2)};
    successors[block + 1] = {uint16_t(block + 3)};
    successors[block + 2] = {uint16_t(block + 3)};
  }
  CfgGraph fixture(successors);
  loom_cfg_regions_t regions;
  IREE_ASSERT_OK(harness_.Build(fixture, &regions));
  for (uint16_t block = 0; block + 3 < count; block += 3) {
    EXPECT_EQ(regions.blocks[block].continuation_index,
              LOOM_CFG_REGION_CONTINUATION_NONE);
    EXPECT_EQ(regions.blocks[block + 1].continuation_index, block + 3);
    EXPECT_EQ(regions.blocks[block + 2].continuation_index, block + 3);
    EXPECT_EQ(regions.blocks[block + 1].exit_edges.count, 1u);
    EXPECT_EQ(regions.blocks[block + 2].exit_edges.count, 1u);
  }
}

}  // namespace
}  // namespace loom
