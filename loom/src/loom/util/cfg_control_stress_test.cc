// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <random>
#include <vector>

#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"
#include "loom/util/cfg_control_test_harness.h"

namespace loom {
namespace {

class CfgControlStressTest : public ::testing::Test {
 protected:
  testing::CfgControlTestHarness harness_;
};

TEST_F(CfgControlStressTest, RandomControlGraphs) {
  std::mt19937 random(541213);
  for (uint32_t trial = 0; trial < 4000; ++trial) {
    SCOPED_TRACE(trial);
    const uint32_t count = 1 + random() % 24;
    std::vector<std::vector<uint16_t>> successors(count);
    for (auto& edges : successors) {
      uint32_t edge_count = random() % 5;
      while (edge_count--) {
        edges.push_back(random() % count);
      }
    }
    IREE_ASSERT_OK(harness_.CheckStructure(successors));
  }
}

TEST_F(CfgControlStressTest, RandomFactUpdates) {
  std::mt19937 random(996137);
  for (uint32_t trial = 0; trial < 3000; ++trial) {
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

TEST_F(CfgControlStressTest, RandomPostdominanceGraphs) {
  uint32_t seed = 4139;
  auto random = [&]() { return seed = seed * 1664525u + 1013904223u; };
  for (int iteration = 0; iteration < 4000; ++iteration) {
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

}  // namespace
}  // namespace loom
