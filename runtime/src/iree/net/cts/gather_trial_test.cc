// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/net/cts/gather_trial.h"

#include "iree/net/cts/collective_trial_test_util.h"
#include "iree/testing/gtest.h"

namespace iree::net::cts {
namespace {

class GatherTrialTest
    : public ::testing::TestWithParam<iree::async::cts::BackendInfo> {
 protected:
  void Run(GatherTrialOptions options) {
    CheckGatherTrial(GetParam().factory, options);
  }
};

TEST_P(GatherTrialTest, TwoRanksUnitWindow) {
  GatherTrialOptions options;
  options.rank_count = 2;
  options.window_size = 1;
  options.block_size = 132;
  options.depth = 1;
  Run(options);
}

TEST_P(GatherTrialTest, NewerHandlesConsumedWhileOlderStorageRemainsLive) {
  GatherTrialOptions options;
  options.window_size = 3;
  options.block_size = 128;
  Run(options);
}

TEST_P(GatherTrialTest, BlocksLargerThanShards) {
  GatherTrialOptions options;
  options.shard_size = 4100;
  options.block_size = 65536;
  options.depth = 5;
  Run(options);
}

CTS_REGISTER_TEST_SUITE(GatherTrialTest);

}  // namespace
}  // namespace iree::net::cts
