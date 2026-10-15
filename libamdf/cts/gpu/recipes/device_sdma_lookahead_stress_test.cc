// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <vector>

#include "libamdf/cts/gpu/recipes/device_sdma_lookahead_fixture.h"

namespace {

TEST_P(DeviceSdmaLookaheadTest, ReuseJoinsEveryIndependentReader) {
  Run(GetParam());
}

std::vector<LookaheadCase> StressCases() {
  std::vector<LookaheadCase> cases;
  for (const amdf_memory_class_t memory_class :
       {AMDF_MEMORY_CLASS_SYSTEM, AMDF_MEMORY_CLASS_LOCAL}) {
    const std::string placement =
        memory_class == AMDF_MEMORY_CLASS_LOCAL ? "Local" : "System";
    for (uint32_t slot_count : {1u, 2u, 4u}) {
      for (uint32_t reader_count : {1u, 2u}) {
        const std::string suffix = "Slots" + std::to_string(slot_count) +
                                   "Readers" + std::to_string(reader_count);
        cases.push_back({memory_class,
                         slot_count,
                         reader_count,
                         {17, 66, 72, 1584},
                         129,
                         placement + "Rows" + suffix});
        cases.push_back({memory_class,
                         slot_count,
                         reader_count,
                         {1024, 4352, 18432, 16384},
                         17,
                         placement + "Blocks" + suffix});
      }
    }
  }
  return cases;
}

INSTANTIATE_TEST_SUITE_P(Placement, DeviceSdmaLookaheadTest,
                         ::testing::ValuesIn(StressCases()), LookaheadCaseName);

}  // namespace
