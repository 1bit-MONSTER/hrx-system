// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <vector>

#include "libamdf/cts/gpu/recipes/device_sdma_staged_fixture.h"

namespace {

TEST_P(DeviceGeneratedSdmaStagedTest, IndependentConsumerReusesSelectedSlots) {
  Run(GetParam());
}

std::vector<DeviceSdmaStagedCase> StressCases() {
  std::vector<DeviceSdmaStagedCase> cases;
  for (const amdf_memory_class_t memory_class :
       {AMDF_MEMORY_CLASS_SYSTEM, AMDF_MEMORY_CLASS_LOCAL}) {
    for (uint32_t slot_count : {1u, 2u, 4u}) {
      const std::string placement =
          memory_class == AMDF_MEMORY_CLASS_LOCAL ? "Local" : "System";
      const std::string suffix = "Slots" + std::to_string(slot_count);
      cases.push_back({memory_class,
                       {17, 66, 72, 1584},
                       97,
                       slot_count,
                       placement + "Rows" + suffix});
      cases.push_back({memory_class,
                       {1024, 4352, 18432, 262144},
                       17,
                       slot_count,
                       placement + "Blocks" + suffix});
    }
  }
  return cases;
}

INSTANTIATE_TEST_SUITE_P(Placement, DeviceGeneratedSdmaStagedTest,
                         ::testing::ValuesIn(StressCases()),
                         DeviceSdmaStagedCaseName);

}  // namespace
