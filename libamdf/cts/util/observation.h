// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef AMDF_CTS_UTIL_OBSERVATION_H_
#define AMDF_CTS_UTIL_OBSERVATION_H_

#include "amdf/amdf.h"
#include "amdf/observation.h"
#include "gtest/gtest.h"

// The same library host domain for CPU, GPU, and NPU timing witnesses.
// Each caller owns this small description; reads retain no calibration state.
class ObservationHostClock {
 public:
  void Initialize(const amdf_api_t* api) {
    const void* extension = nullptr;
    ASSERT_EQ(
        api->query_extension(AMDF_EXTENSION_OBSERVATION, 1, 1, &extension),
        AMDF_STATUS_OK);
    api_ = static_cast<const amdf_observation_api_t*>(extension);
    ASSERT_EQ(api_->query_host_clock_info(&info_), AMDF_STATUS_OK);
    ASSERT_GT(info_.frequency_hz, 0u);
  }

  amdf_status_t Read(uint64_t* out_timestamp) const {
    return api_->query_host_clock(out_timestamp);
  }

  // Native units retained alongside raw host samples in result artifacts.
  uint64_t frequency_hz() const { return info_.frequency_hz; }

  // Convert differences, never absolute epochs, before floating-point scaling.
  double Nanoseconds(uint64_t delta) const {
    return double(delta) * 1e9 / info_.frequency_hz;
  }

 private:
  // Immutable table borrowed from the corpus provider.
  const amdf_observation_api_t* api_ = nullptr;
  // Native host tick units retained for this caller's observation lifetime.
  amdf_observation_clock_info_t info_ = {};
};

#endif  // AMDF_CTS_UTIL_OBSERVATION_H_
