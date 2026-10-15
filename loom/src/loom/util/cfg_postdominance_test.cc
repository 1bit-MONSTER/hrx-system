// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/util/cfg_postdominance.h"

#include <utility>
#include <vector>

#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"
#include "loom/util/cfg_control_test_harness.h"
#include "loom/util/cfg_graph_test_util.h"

namespace loom {
namespace {

using Graph = testing::CfgGraph;

class CfgPostdominanceTest : public ::testing::Test {
 protected:
  testing::CfgControlTestHarness harness_;
};

TEST_F(CfgPostdominanceTest, EmptySingleBlockAndSelfLoop) {
  IREE_ASSERT_OK(harness_.CheckPostdominance({}));
  IREE_ASSERT_OK(harness_.CheckPostdominance({{}}));
  IREE_ASSERT_OK(harness_.CheckPostdominance({{0}}));
}

TEST_F(CfgPostdominanceTest, ReverseChainAndDiamond) {
  IREE_ASSERT_OK(harness_.CheckPostdominance({{3}, {}, {1}, {2}}));
  IREE_ASSERT_OK(harness_.CheckPostdominance({{1, 2}, {3}, {3}, {}}));
}

TEST_F(CfgPostdominanceTest, MultipleExitsAndPartialNontermination) {
  IREE_ASSERT_OK(harness_.CheckPostdominance({{1, 2}, {}, {3, 4}, {}, {}}));
  IREE_ASSERT_OK(harness_.CheckPostdominance({{1, 2}, {1}, {}}));
  IREE_ASSERT_OK(harness_.CheckPostdominance({{1, 2}, {2}, {1}}));
}

TEST_F(CfgPostdominanceTest, LoopsDuplicateEdgesAndUnreachablePredecessors) {
  IREE_ASSERT_OK(
      harness_.CheckPostdominance({{1, 1}, {2, 3}, {1}, {}, {3, 4}}));
  IREE_ASSERT_OK(harness_.CheckPostdominance({{1, 2}, {2, 3}, {1, 3}, {}}));
  IREE_ASSERT_OK(harness_.CheckPostdominance({{}, {2}, {1}}));
}

TEST_F(CfgPostdominanceTest, EveryThreeBlockGraph) {
  for (uint32_t mask = 0; mask < 512; ++mask) {
    SCOPED_TRACE(mask);
    std::vector<std::vector<uint16_t>> edges(3);
    for (uint16_t source = 0; source < 3; ++source) {
      for (uint16_t target = 0; target < 3; ++target) {
        if (mask & (1u << (source * 3 + target))) {
          edges[source].push_back(target);
        }
      }
    }
    IREE_ASSERT_OK(harness_.CheckPostdominance(edges));
  }
}

TEST_F(CfgPostdominanceTest, ArbitraryGraphsAgainstReachability) {
  uint32_t seed = 4139;
  auto random = [&]() { return seed = seed * 1664525u + 1013904223u; };
  // The manual stress test retains the full fixed-seed sequence, while
  // fact_control_fuzz owns unbounded topology search through this analysis.
  for (int iteration = 0; iteration < 256; ++iteration) {
    SCOPED_TRACE(iteration);
    const size_t count = 1 + random() % 24;
    std::vector<std::vector<uint16_t>> edges(count);
    for (auto& successors : edges) {
      const size_t successor_count = random() % 5;
      for (size_t i = 0; i < successor_count; ++i) {
        successors.push_back(random() % count);
      }
    }
    IREE_ASSERT_OK(harness_.CheckPostdominance(edges));
  }
}

TEST_F(CfgPostdominanceTest, FullBlockIndexRangeWithoutRecursiveStack) {
  constexpr size_t count = UINT16_MAX;
  std::vector<std::vector<uint16_t>> edges(count);
  edges[0].push_back(count - 1);
  for (size_t i = count - 1; i > 1; --i) {
    edges[i].push_back(i - 1);
  }
  Graph fixture(std::move(edges));
  loom_cfg_postdominance_t postdominance;
  IREE_ASSERT_OK(loom_cfg_postdominance_build(fixture.get(), harness_.arena(),
                                              &postdominance));
  ASSERT_TRUE(postdominance.available);
  ASSERT_EQ(postdominance.exit_node, count);
  EXPECT_EQ(postdominance.nodes[0].immediate_postdominator, count - 1);
  EXPECT_EQ(postdominance.nodes[0].depth, count);
  EXPECT_EQ(postdominance.nodes[1].immediate_postdominator, count);
  EXPECT_EQ(postdominance.nodes[1].depth, 1u);
  for (size_t i = 2; i < count; ++i) {
    EXPECT_EQ(postdominance.nodes[i].immediate_postdominator, i - 1);
    EXPECT_EQ(postdominance.nodes[i].depth, i);
  }
  EXPECT_EQ(postdominance.nodes[count].immediate_postdominator, count);
  EXPECT_EQ(postdominance.nodes[count].depth, 0u);
}

TEST_F(CfgPostdominanceTest, MalformedGraphIsUnavailable) {
  loom_cfg_graph_t graph = {.malformed = true};
  loom_cfg_postdominance_t postdominance;
  IREE_ASSERT_OK(
      loom_cfg_postdominance_build(&graph, harness_.arena(), &postdominance));
  EXPECT_FALSE(postdominance.available);
  EXPECT_EQ(postdominance.nodes, nullptr);
}

}  // namespace
}  // namespace loom
