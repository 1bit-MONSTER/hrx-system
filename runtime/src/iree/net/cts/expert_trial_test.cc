// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/net/cts/expert_trial.h"

#include "iree/net/cts/collective_trial_test_util.h"
#include "iree/testing/gtest.h"

namespace iree::net::cts {
namespace {

class ExpertTrialTest
    : public ::testing::TestWithParam<iree::async::cts::BackendInfo> {
 protected:
  void Run(ExpertTrialOptions options) {
    CheckExpertTrial(GetParam().factory, options);
  }
};

TEST_P(ExpertTrialTest, TwoRankRoundTrip) {
  ExpertTrialOptions options;
  options.rank_count = 2;
  options.traffic = ExpertTraffic::kUniform;
  options.token_capacity = 3;
  options.depth = 1;
  options.window_size = 1;
  options.block_size = 132;
  Run(options);
}

TEST_P(ExpertTrialTest, UnevenIdleAndRetainedRounds) {
  ExpertTrialOptions options;
  options.block_size = 128;
  options.window_size = 3;
  Run(options);
}

TEST_P(ExpertTrialTest, IdleRankReceivesHotExpertShard) {
  ExpertTrialOptions options;
  options.traffic = ExpertTraffic::kHotspot;
  options.token_capacity = 7;
  options.block_size = 128;
  options.window_size = 1;
  Run(options);
}

TEST_P(ExpertTrialTest, EmptyCountsStillParticipate) {
  ExpertTrialOptions options;
  options.token_capacity = 0;
  options.traffic = ExpertTraffic::kUniform;
  options.block_size = 16;
  options.window_size = 1;
  options.depth = 3;
  Run(options);
}

CTS_REGISTER_TEST_SUITE(ExpertTrialTest);

}  // namespace
}  // namespace iree::net::cts
