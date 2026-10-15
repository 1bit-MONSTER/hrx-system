// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <random>
#include <vector>

#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"
#include "loom/util/cfg_regions_test_util.h"

namespace loom {
namespace {

class CfgRegionsStressTest : public ::testing::Test {
 protected:
  testing::CfgRegionsTestHarness harness_;
};

TEST_F(CfgRegionsStressTest, EveryFourBlockGraph) {
  for (uint32_t bits = 0; bits < (1u << 16); ++bits) {
    SCOPED_TRACE(bits);
    std::vector<std::vector<uint16_t>> successors(4);
    for (uint16_t source = 0; source < 4; ++source) {
      for (uint16_t target = 0; target < 4; ++target) {
        if (bits & (1u << (source * 4 + target))) {
          successors[source].push_back(target);
        }
      }
    }
    IREE_ASSERT_OK(harness_.Check(successors));
  }
}

TEST_F(CfgRegionsStressTest, DeterministicArbitraryGraphs) {
  std::mt19937 random(634);
  for (size_t sample = 0; sample < 2000; ++sample) {
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

}  // namespace
}  // namespace loom
