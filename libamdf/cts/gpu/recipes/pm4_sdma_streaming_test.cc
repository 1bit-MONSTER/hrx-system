// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "libamdf/cts/gpu/recipes/pm4_sdma_streaming_fixture.h"

namespace {

constexpr StreamingWorkload kPresubmitWorkload = {
    .minimum_graph_count = 24,
    .native_ring_traversal_count = 0,
};

TEST_F(Pm4SdmaStreamingTest, CoherentHostStreaming) {
  RunStreaming(kPresubmitWorkload, AcquireMode::kFullBarrier);
}

TEST_F(Pm4SdmaStreamingTest, OrderedDataAcquireHostStreaming) {
  RunStreaming(kPresubmitWorkload, AcquireMode::kOrderedData);
}

TEST_F(Pm4SdmaStreamingTest, CreditTokensCrossHighBit) {
  RunStreaming(kPresubmitWorkload, AcquireMode::kFullBarrier,
               UINT32_C(0x7ffffffe));
}

TEST_F(Pm4SdmaStreamingTest, OrderedDataAcquireCreditTokensWrap) {
  RunStreaming(kPresubmitWorkload, AcquireMode::kOrderedData,
               UINT32_C(0xfffffffe));
}

TEST_F(Pm4SdmaStreamingTest, ProducerClosureWithPendingIngress) {
  RunStreaming(kPresubmitWorkload, AcquireMode::kFullBarrier, 0,
               ProducerClosure::kPendingIngress);
}

TEST_F(Pm4SdmaStreamingTest, OrderedDataAcquireProducerClosureWithUnusedInput) {
  RunStreaming(kPresubmitWorkload, AcquireMode::kOrderedData, 0,
               ProducerClosure::kUnusedInput);
}

}  // namespace
