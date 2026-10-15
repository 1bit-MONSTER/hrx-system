// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/net/cts/collective_trial.h"

#include "iree/net/cts/collective_trial_test_util.h"
#include "iree/testing/gtest.h"

namespace iree::net::cts {
namespace {

class CollectiveTrialTest
    : public ::testing::TestWithParam<iree::async::cts::BackendInfo> {
 protected:
  void Run(CollectiveTrialOptions options) {
    CheckCollectiveTrial(GetParam().factory, options);
  }
};

TEST_P(CollectiveTrialTest, TwoRankUnitWindow) {
  CollectiveTrialOptions options;
  options.window_size = 1;
  options.block_size = 128;
  options.measured_rounds = 5;
  Run(options);
}

TEST_P(CollectiveTrialTest, MultipleRanksAndPartialBlocks) {
  CollectiveTrialOptions options;
  options.rank_count = 3;
  options.tensor_size = 12288;
  options.block_size = 132;
  options.window_size = 7;
  options.measured_rounds = 5;
  Run(options);
}

TEST_P(CollectiveTrialTest, BlocksLargerThanShards) {
  CollectiveTrialOptions options;
  options.rank_count = 4;
  options.tensor_size = 65536;
  options.block_size = 32768;
  options.window_size = 3;
  options.measured_rounds = 3;
  Run(options);
}

TEST_P(CollectiveTrialTest, PipelineActivationLargerThanSendWindow) {
  CollectiveTrialOptions options;
  options.schedule = CollectiveSchedule::kPipeline;
  options.tensor_size = 4100;
  options.block_size = 128;
  options.window_size = 1;
  options.measured_rounds = 5;
  Run(options);
}

TEST_P(CollectiveTrialTest, PipelineOverlapAndPartialBlocks) {
  CollectiveTrialOptions options;
  options.schedule = CollectiveSchedule::kPipeline;
  options.rank_count = 4;
  options.tensor_size = 65540;
  options.block_size = 4096;
  options.window_size = 7;
  options.pipeline_depth = 3;
  options.measured_rounds = 11;
  Run(options);
}

TEST_P(CollectiveTrialTest, PipelineActivationSpansReceivePool) {
  CollectiveTrialOptions options;
  options.schedule = CollectiveSchedule::kPipeline;
  options.rank_count = 3;
  options.tensor_size = 65540;
  options.block_size = 128;
  options.window_size = 32;
  options.pipeline_depth = 2;
  options.measured_rounds = 3;
  Run(options);
}

TEST_P(CollectiveTrialTest, PipelineBlocksLargerThanActivations) {
  CollectiveTrialOptions options;
  options.schedule = CollectiveSchedule::kPipeline;
  options.rank_count = 3;
  options.tensor_size = 8196;
  options.block_size = 65536;
  options.window_size = 3;
  options.pipeline_depth = 5;
  options.measured_rounds = 7;
  Run(options);
}

CTS_REGISTER_TEST_SUITE(CollectiveTrialTest);

}  // namespace
}  // namespace iree::net::cts
