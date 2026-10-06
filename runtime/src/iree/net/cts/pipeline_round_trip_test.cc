// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/net/cts/pipeline_round_trip.h"

#include <algorithm>

#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"

namespace iree::net::cts {
namespace {

class PipelineRoundTripTest
    : public ::testing::TestWithParam<iree::async::cts::BackendInfo> {
 protected:
  void Run(PipelineRoundTripOptions options) {
    for (auto delivery :
         {CollectiveDelivery::kMessage, CollectiveDelivery::kRegistered}) {
      if (delivery == CollectiveDelivery::kRegistered &&
          !GetTransportBackend().create_registered_factory) {
        continue;
      }
      SCOPED_TRACE(delivery == CollectiveDelivery::kMessage ? "message"
                                                            : "registered");
      options.delivery = delivery;
      PipelineRoundTripResult result;
      iree_status_t status = RunPipelineRoundTrip(
          GetTransportBackend(), GetParam().factory, options, &result);
      if (!result.available && iree_status_is_unavailable(status)) {
        std::string reason = iree::Status::ToString(status);
        iree_status_free(status);
        GTEST_SKIP() << reason;
      }
      IREE_ASSERT_OK(status);
      EXPECT_GT(result.control_sends, 0u);
      EXPECT_EQ(result.control_completions, result.control_sends);
      size_t forward_chunks =
          1 + (options.activation_size - 1) / options.block_size;
      size_t backward_chunks =
          1 + (options.gradient_size - 1) / options.block_size;
      uint64_t edges = options.rank_count - 1;
      EXPECT_EQ(result.microbatches, options.measured_rounds);
      EXPECT_EQ(result.sends, options.measured_rounds * edges *
                                  (forward_chunks + backward_chunks));
      EXPECT_EQ(result.source_completions, result.sends);
      EXPECT_EQ(result.payload_bytes,
                options.measured_rounds * edges *
                    (options.activation_size + options.gradient_size));
      EXPECT_LE(result.source_window_high_water, options.window_size);
      EXPECT_LE(result.window_high_water,
                options.depth * std::max(forward_chunks, backward_chunks));
      EXPECT_GT(result.pipeline_high_water, 0u);
      EXPECT_LE(result.pipeline_high_water, options.depth);
      EXPECT_GT(result.first_completion_seconds, 0.0);
      EXPECT_LE(result.first_completion_seconds, result.elapsed_seconds);
    }
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

TEST_P(PipelineRoundTripTest, BothDirectionsSpanReceivePool) {
  PipelineRoundTripOptions options;
  options.rank_count = 3;
  options.activation_size = 65540;
  options.gradient_size = 65544;
  options.block_size = 128;
  options.window_size = 32;
  options.depth = 2;
  options.warmup_rounds = 1;
  options.measured_rounds = 5;
  Run(options);
}

TEST_P(PipelineRoundTripTest, EightStagesAndLargeBlocks) {
  PipelineRoundTripOptions options;
  options.rank_count = 8;
  options.activation_size = 8196;
  options.gradient_size = 65540;
  options.block_size = 65536;
  options.window_size = 7;
  options.depth = 4;
  options.warmup_rounds = 1;
  options.measured_rounds = 7;
  Run(options);
}

TEST_P(PipelineRoundTripTest, LargeActivationsAndRepeatedSlotReuse) {
  PipelineRoundTripOptions options;
  options.activation_size = 1024 * 1024 + 4;
  options.gradient_size = 512 * 1024 + 4;
  options.block_size = 65536;
  options.window_size = 65;
  options.warmup_rounds = 1;
  options.measured_rounds = 7;
  Run(options);
}

CTS_REGISTER_TEST_SUITE(PipelineRoundTripTest);

}  // namespace
}  // namespace iree::net::cts
