// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "libamdf/cts/gpu/recipes/device_sdma_lookahead_fixture.h"

namespace {

TEST_P(DeviceSdmaLookaheadTest, ReuseJoinsEveryIndependentReader) {
  Run(GetParam());
}

INSTANTIATE_TEST_SUITE_P(
    Placement, DeviceSdmaLookaheadTest,
    ::testing::Values(LookaheadCase{AMDF_MEMORY_CLASS_SYSTEM,
                                    1,
                                    1,
                                    {17, 66, 72, 1584},
                                    17,
                                    "SystemRowsSlots1Readers1"},
                      LookaheadCase{AMDF_MEMORY_CLASS_SYSTEM,
                                    2,
                                    2,
                                    {1024, 4352, 18432, 16384},
                                    17,
                                    "SystemBlocksSlots2Readers2"},
                      LookaheadCase{AMDF_MEMORY_CLASS_SYSTEM,
                                    4,
                                    2,
                                    {17, 66, 72, 1584},
                                    17,
                                    "SystemRowsSlots4Readers2"},
                      LookaheadCase{AMDF_MEMORY_CLASS_LOCAL,
                                    1,
                                    2,
                                    {1024, 4352, 18432, 16384},
                                    17,
                                    "LocalBlocksSlots1Readers2"},
                      LookaheadCase{AMDF_MEMORY_CLASS_LOCAL,
                                    2,
                                    1,
                                    {17, 66, 72, 1584},
                                    17,
                                    "LocalRowsSlots2Readers1"},
                      LookaheadCase{AMDF_MEMORY_CLASS_LOCAL,
                                    4,
                                    1,
                                    {1024, 4352, 18432, 16384},
                                    17,
                                    "LocalBlocksSlots4Readers1"}),
    LookaheadCaseName);

}  // namespace
