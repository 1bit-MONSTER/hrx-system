// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "amdf/observation.h"

#include <chrono>
#include <cstdint>
#include <thread>

#include "gtest/gtest.h"
#include "util/provider.h"

namespace {

class ObservationTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(amdf_cts_provider_query_api()(AMDF_ABI_VERSION_LATEST,
                                            AMDF_ABI_VERSION_LATEST, &api_),
              AMDF_STATUS_OK);
    const void* extension = nullptr;
    ASSERT_EQ(
        api_->query_extension(AMDF_EXTENSION_OBSERVATION, 1, 1, &extension),
        AMDF_STATUS_OK);
    observation_ = static_cast<const amdf_observation_api_t*>(extension);
  }
  // Borrowed provider table; no instance or device is created by these cases.
  const amdf_api_t* api_ = nullptr;
  // Borrowed immutable timing services.
  const amdf_observation_api_t* observation_ = nullptr;
};

TEST_F(ObservationTest, HostClockNeedsNoDeviceAndMeasuresElapsedWork) {
  ASSERT_EQ(observation_->structure_size, sizeof(amdf_observation_api_t));
  amdf_observation_clock_info_t clock = {};
  ASSERT_EQ(observation_->query_host_clock_info(&clock), AMDF_STATUS_OK);
  ASSERT_GT(clock.frequency_hz, 0u);
  ASSERT_EQ(clock.counter_bit_count, 64u);
  ASSERT_EQ(clock.reserved, 0u);
  const auto outer_begin = std::chrono::steady_clock::now();
  uint64_t begin = 0;
  ASSERT_EQ(observation_->query_host_clock(&begin), AMDF_STATUS_OK);
  std::this_thread::sleep_for(std::chrono::milliseconds(2));
  uint64_t end = 0;
  ASSERT_EQ(observation_->query_host_clock(&end), AMDF_STATUS_OK);
  const auto outer_end = std::chrono::steady_clock::now();
  ASSERT_GT(end, begin);
  const double elapsed = double(end - begin) / clock.frequency_hz;
  const double outer =
      std::chrono::duration<double>(outer_end - outer_begin).count();
  EXPECT_GE(elapsed, 0.001);
  // Independent host observations catch unit errors without a scheduler limit.
  EXPECT_LE(elapsed, outer * 1.01);
}

TEST_F(ObservationTest, RejectsInvalidOutputsAndUnsupportedVersions) {
  EXPECT_EQ(observation_->query_host_clock(nullptr),
            amdf_make_api_status(AMDF_STATUS_CODE_INVALID_ARGUMENT));
  EXPECT_EQ(observation_->query_host_clock_info(nullptr),
            amdf_make_api_status(AMDF_STATUS_CODE_INVALID_ARGUMENT));
  const void* extension = observation_;
  EXPECT_EQ(api_->query_extension(AMDF_EXTENSION_OBSERVATION, 2, 2, &extension),
            amdf_make_api_status(AMDF_STATUS_CODE_VERSION_MISMATCH));
  EXPECT_EQ(extension, observation_);
}

}  // namespace
