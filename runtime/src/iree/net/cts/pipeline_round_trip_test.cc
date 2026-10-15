// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/net/cts/pipeline_round_trip.h"

#include "iree/net/cts/collective_trial_test_util.h"
#include "iree/testing/gtest.h"

namespace iree::net::cts {
namespace {

class PipelineRoundTripTest
    : public ::testing::TestWithParam<iree::async::cts::BackendInfo> {
 protected:
  void Run(PipelineRoundTripOptions options) {
    CheckPipelineRoundTrip(GetParam().factory, options);
  }
};

TEST_P(PipelineRoundTripTest, TwoStagesUnitWindowAndAsymmetricReturn) {
  PipelineRoundTripOptions options;
  options.rank_count = 2;
  options.depth = 1;
  options.window_size = 1;
  options.block_size = 132;
  options.activation_size = 4100;
  options.gradient_size = 8196;
  options.measured_rounds = 5;
  Run(options);
}

TEST_P(PipelineRoundTripTest, RetainedActivationsAcrossFourStages) {
  PipelineRoundTripOptions options;
  options.activation_size = 65540;
  options.gradient_size = 32772;
  options.block_size = 4096;
  options.window_size = 7;
  Run(options);
}

CTS_REGISTER_TEST_SUITE(PipelineRoundTripTest);

}  // namespace
}  // namespace iree::net::cts
