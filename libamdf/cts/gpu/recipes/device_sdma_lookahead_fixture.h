// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef AMDF_CTS_GPU_RECIPES_DEVICE_SDMA_LOOKAHEAD_FIXTURE_H_
#define AMDF_CTS_GPU_RECIPES_DEVICE_SDMA_LOOKAHEAD_FIXTURE_H_

#include <array>
#include <cstdint>
#include <string>

#include "libamdf/cts/gpu/recipes/device_sdma_fixture.h"

struct LookaheadCase {
  // Exact required input placement; LOCAL never substitutes SYSTEM backing.
  amdf_memory_class_t memory_class;
  // Number of reusable input slots, controlling the maximum lookahead.
  uint32_t slot_count;
  // Number of independent readers whose completion permits slot reuse.
  uint32_t reader_count;
  // Four positive word counts selected by device computation.
  std::array<uint32_t, 4> word_counts;
  // Finite number of jobs published before the first upload starts.
  uint32_t job_count;
  // Stable placement, size, slot, and reader parameter name.
  std::string name;
};

class DeviceSdmaLookaheadTest
    : public DeviceGeneratedSdmaTest,
      public ::testing::WithParamInterface<LookaheadCase> {
 protected:
  void Run(const LookaheadCase& test_case);
};

std::string LookaheadCaseName(
    const ::testing::TestParamInfo<LookaheadCase>& info);

#endif  // AMDF_CTS_GPU_RECIPES_DEVICE_SDMA_LOOKAHEAD_FIXTURE_H_
