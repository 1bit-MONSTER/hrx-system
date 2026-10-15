// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/net/cts/collective_trial_test_util.h"
#include "iree/testing/gtest.h"

namespace iree::net::cts {
namespace {

class CollectiveTrialStressTest
    : public ::testing::TestWithParam<iree::async::cts::BackendInfo> {};

TEST_P(CollectiveTrialStressTest, LargeTensorsAndBoundedNativeWindows) {
  CollectiveTrialOptions options;
  options.rank_count = 4;
  options.tensor_size = 1024 * 1024;
  options.block_size = 4096;
  options.window_size = 65;
  options.warmup_rounds = 1;
  options.measured_rounds = 2;
  CheckCollectiveTrial(GetParam().factory, options);
}

TEST_P(CollectiveTrialStressTest, PipelineLargeActivations) {
  CollectiveTrialOptions options;
  options.schedule = CollectiveSchedule::kPipeline;
  options.rank_count = 4;
  options.tensor_size = 1024 * 1024 + 4;
  options.block_size = 65536;
  options.window_size = 65;
  options.pipeline_depth = 4;
  options.warmup_rounds = 1;
  options.measured_rounds = 5;
  CheckCollectiveTrial(GetParam().factory, options);
}

CTS_REGISTER_TEST_SUITE(CollectiveTrialStressTest);

class ExpertTrialStressTest
    : public ::testing::TestWithParam<iree::async::cts::BackendInfo> {};

TEST_P(ExpertTrialStressTest, EightRanksAndWideAsymmetricTokens) {
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
  CheckExpertTrial(GetParam().factory, options);
}

TEST_P(ExpertTrialStressTest, FramesSpanReceivePoolWithoutRetainingLeases) {
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
  CheckExpertTrial(GetParam().factory, options);
}

CTS_REGISTER_TEST_SUITE(ExpertTrialStressTest);

class GatherTrialStressTest
    : public ::testing::TestWithParam<iree::async::cts::BackendInfo> {};

TEST_P(GatherTrialStressTest, RetainedShardsSpanReceivePool) {
  GatherTrialOptions options;
  options.rank_count = 2;
  options.shard_size = 65540;
  options.block_size = 128;
  options.window_size = 65;
  CheckGatherTrial(GetParam().factory, options);
}

TEST_P(GatherTrialStressTest, EightRanksAndPartialSessionTail) {
  GatherTrialOptions options;
  options.rank_count = 8;
  options.shard_size = 65540;
  options.block_size = 65536;
  options.window_size = 7;
  options.depth = 4;
  options.warmup_rounds = 1;
  options.measured_rounds = 5;
  CheckGatherTrial(GetParam().factory, options);
}

CTS_REGISTER_TEST_SUITE(GatherTrialStressTest);

class PipelineRoundTripStressTest
    : public ::testing::TestWithParam<iree::async::cts::BackendInfo> {};

TEST_P(PipelineRoundTripStressTest, BothDirectionsSpanReceivePool) {
  PipelineRoundTripOptions options;
  options.rank_count = 3;
  options.activation_size = 65540;
  options.gradient_size = 65544;
  options.block_size = 128;
  options.window_size = 32;
  options.depth = 2;
  options.warmup_rounds = 1;
  options.measured_rounds = 5;
  CheckPipelineRoundTrip(GetParam().factory, options);
}

TEST_P(PipelineRoundTripStressTest, EightStagesAndLargeBlocks) {
  PipelineRoundTripOptions options;
  options.rank_count = 8;
  options.activation_size = 8196;
  options.gradient_size = 65540;
  options.block_size = 65536;
  options.window_size = 7;
  options.depth = 4;
  options.warmup_rounds = 1;
  options.measured_rounds = 7;
  CheckPipelineRoundTrip(GetParam().factory, options);
}

TEST_P(PipelineRoundTripStressTest, LargeActivationsAndRepeatedSlotReuse) {
  PipelineRoundTripOptions options;
  options.activation_size = 1024 * 1024 + 4;
  options.gradient_size = 512 * 1024 + 4;
  options.block_size = 65536;
  options.window_size = 65;
  options.warmup_rounds = 1;
  options.measured_rounds = 7;
  CheckPipelineRoundTrip(GetParam().factory, options);
}

CTS_REGISTER_TEST_SUITE(PipelineRoundTripStressTest);

}  // namespace
}  // namespace iree::net::cts
