// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/net/cts/gather_trial.h"

#include <algorithm>

#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"

namespace iree::net::cts {
namespace {

class GatherTrialTest
    : public ::testing::TestWithParam<iree::async::cts::BackendInfo> {
 protected:
  void Run(GatherTrialOptions options) {
    for (auto delivery :
         {CollectiveDelivery::kMessage, CollectiveDelivery::kRegistered}) {
      if (delivery == CollectiveDelivery::kRegistered &&
          !GetTransportBackend().create_registered_factory) {
        continue;
      }
      SCOPED_TRACE(delivery == CollectiveDelivery::kMessage ? "message"
                                                            : "registered");
      options.delivery = delivery;
      GatherTrialResult result;
      iree_status_t status = RunGatherTrial(
          GetTransportBackend(), GetParam().factory, options, &result);
      if (!result.available && iree_status_is_unavailable(status)) {
        std::string reason = iree::Status::ToString(status);
        iree_status_free(status);
        GTEST_SKIP() << reason;
      }
      IREE_ASSERT_OK(status);
      EXPECT_GT(result.control_sends, 0u);
      EXPECT_EQ(result.control_completions, result.control_sends);
      size_t chunks = 1 + (options.shard_size - 1) / options.block_size;
      uint64_t edges = uint64_t(options.rank_count) * (options.rank_count - 1);
      EXPECT_EQ(result.gathers, options.measured_rounds);
      EXPECT_EQ(result.sends, options.measured_rounds * edges * chunks);
      EXPECT_EQ(result.source_completions, result.sends);
      EXPECT_EQ(result.payload_bytes,
                options.measured_rounds * edges * options.shard_size);
      EXPECT_LE(result.source_window_high_water, options.window_size);
      EXPECT_LE(result.window_high_water, options.depth * chunks);
      EXPECT_EQ(result.handles_high_water,
                std::min(options.depth, options.measured_rounds));
      EXPECT_EQ(
          result.out_of_order_reads,
          uint64_t(options.rank_count) *
              (options.measured_rounds -
               (options.measured_rounds + options.depth - 1) / options.depth));
      EXPECT_EQ(result.payload_storage_bytes,
                options.rank_count * options.depth *
                    (options.shard_size +
                     (options.rank_count - 1) * chunks * options.block_size));
    }
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

TEST_P(GatherTrialTest, RetainedShardsSpanReceivePool) {
  GatherTrialOptions options;
  options.rank_count = 2;
  options.shard_size = 65540;
  options.block_size = 128;
  options.window_size = 65;
  Run(options);
}

TEST_P(GatherTrialTest, EightRanksAndPartialSessionTail) {
  GatherTrialOptions options;
  options.rank_count = 8;
  options.shard_size = 65540;
  options.block_size = 65536;
  options.window_size = 7;
  options.depth = 4;
  options.warmup_rounds = 1;
  options.measured_rounds = 5;
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
