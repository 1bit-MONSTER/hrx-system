// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/net/cts/collective_trial.h"

#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"

namespace iree::net::cts {
namespace {

class CollectiveTrialTest
    : public ::testing::TestWithParam<iree::async::cts::BackendInfo> {
 protected:
  void Run(CollectiveTrialOptions options) {
    for (auto delivery :
         {CollectiveDelivery::kMessage, CollectiveDelivery::kRegistered}) {
      if (delivery == CollectiveDelivery::kRegistered &&
          !GetTransportBackend().create_registered_factory) {
        continue;
      }
      SCOPED_TRACE(delivery == CollectiveDelivery::kMessage ? "message"
                                                            : "registered");
      options.delivery = delivery;
      CollectiveTrialResult result;
      iree_status_t status = RunCollectiveTrial(
          GetTransportBackend(), GetParam().factory, options, &result);
      if (!result.available && iree_status_is_unavailable(status)) {
        std::string reason = iree::Status::ToString(status);
        iree_status_free(status);
        GTEST_SKIP() << reason;
      }
      IREE_ASSERT_OK(status);
      EXPECT_GT(result.control_sends, 0u);
      EXPECT_EQ(result.control_completions, result.control_sends);
      EXPECT_GT(result.source_window_high_water, 0u);
      EXPECT_LE(result.source_window_high_water, options.window_size);
      if (options.schedule == CollectiveSchedule::kPipeline) {
        size_t chunks = 1 + (options.tensor_size - 1) / options.block_size;
        EXPECT_EQ(result.microbatches, options.measured_rounds);
        EXPECT_EQ(result.collectives, 0u);
        EXPECT_EQ(result.sends,
                  options.measured_rounds * (options.rank_count - 1) * chunks);
        EXPECT_EQ(result.source_completions, result.sends);
        EXPECT_EQ(result.payload_bytes, options.measured_rounds *
                                            (options.rank_count - 1) *
                                            options.tensor_size);
        EXPECT_GT(result.window_high_water, 0u);
        EXPECT_LE(result.window_high_water, chunks * options.pipeline_depth);
        EXPECT_GT(result.pipeline_high_water, 0u);
        EXPECT_LE(result.pipeline_high_water, options.pipeline_depth);
        EXPECT_GT(result.first_completion_seconds, 0.0);
        EXPECT_LE(result.first_completion_seconds, result.elapsed_seconds);
        continue;
      }
      size_t chunks = 1 + (options.tensor_size / options.rank_count - 1) /
                              options.block_size;
      EXPECT_EQ(result.collectives, options.measured_rounds);
      EXPECT_EQ(result.sends, options.measured_rounds * options.rank_count * 2 *
                                  (options.rank_count - 1) * chunks);
      EXPECT_EQ(result.source_completions, result.sends);
      EXPECT_EQ(result.payload_bytes, options.measured_rounds * 2 *
                                          (options.rank_count - 1) *
                                          options.tensor_size);
      EXPECT_GT(result.window_high_water, 0u);
      EXPECT_LE(result.window_high_water, options.window_size);
    }
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

TEST_P(CollectiveTrialTest, LargeTensorsAndBoundedNativeWindows) {
  CollectiveTrialOptions options;
  options.rank_count = 4;
  options.tensor_size = 1024 * 1024;
  options.block_size = 4096;
  options.window_size = 65;
  options.warmup_rounds = 1;
  options.measured_rounds = 2;
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

TEST_P(CollectiveTrialTest, PipelineLargeActivations) {
  CollectiveTrialOptions options;
  options.schedule = CollectiveSchedule::kPipeline;
  options.rank_count = 4;
  options.tensor_size = 1024 * 1024 + 4;
  options.block_size = 65536;
  options.window_size = 65;
  options.pipeline_depth = 4;
  options.warmup_rounds = 1;
  options.measured_rounds = 5;
  Run(options);
}

CTS_REGISTER_TEST_SUITE(CollectiveTrialTest);

}  // namespace
}  // namespace iree::net::cts
