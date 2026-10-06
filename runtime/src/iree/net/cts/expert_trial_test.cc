// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/net/cts/expert_trial.h"

#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"

namespace iree::net::cts {
namespace {

class ExpertTrialTest
    : public ::testing::TestWithParam<iree::async::cts::BackendInfo> {
 protected:
  void Run(ExpertTrialOptions options) {
    for (auto delivery :
         {CollectiveDelivery::kMessage, CollectiveDelivery::kRegistered}) {
      if (delivery == CollectiveDelivery::kRegistered &&
          !GetTransportBackend().create_registered_factory) {
        continue;
      }
      SCOPED_TRACE(delivery == CollectiveDelivery::kMessage ? "message"
                                                            : "registered");
      options.delivery = delivery;
      ExpertTrialResult result;
      iree_status_t status = RunExpertTrial(
          GetTransportBackend(), GetParam().factory, options, &result);
      if (!result.available && iree_status_is_unavailable(status)) {
        std::string reason = iree::Status::ToString(status);
        iree_status_free(status);
        GTEST_SKIP() << reason;
      }
      IREE_ASSERT_OK(status);
      EXPECT_GT(result.control_sends, 0u);
      EXPECT_EQ(result.control_completions, result.control_sends);
      EXPECT_EQ(result.rounds, options.measured_rounds);
      EXPECT_EQ(result.sends, result.source_completions);
      EXPECT_GT(result.sends, 0u);
      EXPECT_LE(result.source_window_high_water, options.window_size);
      EXPECT_EQ(result.dispatch_bytes,
                result.remote_tokens * options.dispatch_bytes);
      EXPECT_EQ(result.scale_bytes, result.remote_tokens * options.scale_bytes);
      EXPECT_EQ(result.combine_bytes,
                result.remote_tokens * options.combine_bytes);
      EXPECT_EQ(result.header_bytes, uint64_t(options.measured_rounds) *
                                         options.rank_count *
                                         (options.rank_count - 1) * 2 * 16);
      EXPECT_EQ(result.payload_bytes,
                result.dispatch_bytes + result.scale_bytes +
                    result.combine_bytes + result.metadata_bytes +
                    result.header_bytes);
      EXPECT_EQ(
          result.retained_handles,
          uint64_t(options.rank_count) *
              (options.measured_rounds -
               (options.measured_rounds + options.depth - 1) / options.depth));
      if (options.traffic == ExpertTraffic::kUniform) {
        EXPECT_EQ(result.tokens, uint64_t(options.measured_rounds) *
                                     options.rank_count *
                                     options.token_capacity);
      }
      if (options.traffic == ExpertTraffic::kHotspot) {
        EXPECT_EQ(result.maximum_receiver_tokens,
                  uint64_t(options.rank_count - 1) * options.token_capacity);
        EXPECT_EQ(result.tokens, result.remote_tokens);
        EXPECT_EQ(result.local_routes, 0u);
      }
    }
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

TEST_P(ExpertTrialTest, EightRanksAndWideAsymmetricTokens) {
  ExpertTrialOptions options;
  options.rank_count = 8;
  options.expert_count = 384;
  options.token_capacity = 3;
  options.dispatch_bytes = 5120;
  options.scale_bytes = 160;
  options.combine_bytes = 10240;
  options.block_size = 4096;
  options.window_size = 7;
  options.warmup_rounds = 1;
  options.measured_rounds = 3;
  Run(options);
}

TEST_P(ExpertTrialTest, FramesSpanReceivePoolWithoutRetainingLeases) {
  ExpertTrialOptions options;
  options.rank_count = 2;
  options.token_capacity = 65;
  options.dispatch_bytes = 4096;
  options.scale_bytes = 0;
  options.combine_bytes = 8192;
  options.block_size = 128;
  options.window_size = 32;
  options.traffic = ExpertTraffic::kHotspot;
  options.warmup_rounds = 1;
  options.measured_rounds = 3;
  Run(options);
}

CTS_REGISTER_TEST_SUITE(ExpertTrialTest);

}  // namespace
}  // namespace iree::net::cts
