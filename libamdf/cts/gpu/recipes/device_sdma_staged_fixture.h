// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef AMDF_CTS_GPU_RECIPES_DEVICE_SDMA_STAGED_FIXTURE_H_
#define AMDF_CTS_GPU_RECIPES_DEVICE_SDMA_STAGED_FIXTURE_H_

#include <array>
#include <cstdint>
#include <string>

#include "libamdf/cts/gpu/recipes/device_sdma_fixture.h"

struct DeviceSdmaStagedCase {
  // Memory placement required for both reusable payload allocations.
  amdf_memory_class_t memory_class;
  // Positive payload lengths selected by GPU computation, in words.
  std::array<uint32_t, 4> word_counts;
  // Number of separately dispatched upload, compute, and download jobs.
  uint32_t job_count;
  // Number of reusable input/output pairs, a power of two.
  uint32_t slot_count;
  // Stable parameter name including placement, size family, and slot count.
  std::string name;
};

class DeviceGeneratedSdmaStagedTest
    : public DeviceGeneratedSdmaTest,
      public ::testing::WithParamInterface<DeviceSdmaStagedCase> {
 protected:
  void Run(const DeviceSdmaStagedCase& test_case);
};

std::string DeviceSdmaStagedCaseName(
    const ::testing::TestParamInfo<DeviceSdmaStagedCase>& info);

#endif  // AMDF_CTS_GPU_RECIPES_DEVICE_SDMA_STAGED_FIXTURE_H_
