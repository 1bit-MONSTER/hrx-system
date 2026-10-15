// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "libamdf/cts/gpu/recipes/device_sdma_staged_fixture.h"

namespace {

TEST_P(DeviceGeneratedSdmaStagedTest, IndependentConsumerReusesSelectedSlots) {
  Run(GetParam());
}

INSTANTIATE_TEST_SUITE_P(
    Placement, DeviceGeneratedSdmaStagedTest,
    ::testing::Values(DeviceSdmaStagedCase{AMDF_MEMORY_CLASS_SYSTEM,
                                           {17, 66, 72, 1584},
                                           17,
                                           1,
                                           "SystemRowsSlots1"},
                      DeviceSdmaStagedCase{AMDF_MEMORY_CLASS_SYSTEM,
                                           {1024, 4352, 18432, 262144},
                                           17,
                                           2,
                                           "SystemBlocksSlots2"},
                      DeviceSdmaStagedCase{AMDF_MEMORY_CLASS_SYSTEM,
                                           {17, 66, 72, 1584},
                                           17,
                                           4,
                                           "SystemRowsSlots4"},
                      DeviceSdmaStagedCase{AMDF_MEMORY_CLASS_LOCAL,
                                           {1024, 4352, 18432, 262144},
                                           17,
                                           1,
                                           "LocalBlocksSlots1"},
                      DeviceSdmaStagedCase{AMDF_MEMORY_CLASS_LOCAL,
                                           {17, 66, 72, 1584},
                                           17,
                                           2,
                                           "LocalRowsSlots2"},
                      DeviceSdmaStagedCase{AMDF_MEMORY_CLASS_LOCAL,
                                           {1024, 4352, 18432, 262144},
                                           17,
                                           4,
                                           "LocalBlocksSlots4"}),
    DeviceSdmaStagedCaseName);

}  // namespace
