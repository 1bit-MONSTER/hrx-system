// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "libamdf/cts/gpu/recipes/pm4_sdma_streaming_fixture.h"

namespace {

constexpr StreamingWorkload kStressWorkload = {
    .minimum_graph_count = 64,
    .native_ring_traversal_count = 3,
};

TEST_F(Pm4SdmaStreamingTest, CoherentHostStreaming) {
  RunStreaming(kStressWorkload, AcquireMode::kFullBarrier);
}

TEST_F(Pm4SdmaStreamingTest, OrderedDataAcquireHostStreaming) {
  RunStreaming(kStressWorkload, AcquireMode::kOrderedData);
}

TEST_F(Pm4SdmaStreamingTest, CreditTokensCrossHighBit) {
  RunStreaming(kStressWorkload, AcquireMode::kFullBarrier,
               UINT32_C(0x7ffffffe));
}

TEST_F(Pm4SdmaStreamingTest, OrderedDataAcquireCreditTokensCrossHighBit) {
  RunStreaming(kStressWorkload, AcquireMode::kOrderedData,
               UINT32_C(0x7ffffffe));
}

TEST_F(Pm4SdmaStreamingTest, CreditTokensWrap) {
  RunStreaming(kStressWorkload, AcquireMode::kFullBarrier,
               UINT32_C(0xfffffffe));
}

TEST_F(Pm4SdmaStreamingTest, OrderedDataAcquireCreditTokensWrap) {
  RunStreaming(kStressWorkload, AcquireMode::kOrderedData,
               UINT32_C(0xfffffffe));
}

TEST_F(Pm4SdmaStreamingTest, ProducerClosureWithPendingIngress) {
  RunStreaming(kStressWorkload, AcquireMode::kFullBarrier, 0,
               ProducerClosure::kPendingIngress);
}

TEST_F(Pm4SdmaStreamingTest,
       OrderedDataAcquireProducerClosureWithPendingIngress) {
  RunStreaming(kStressWorkload, AcquireMode::kOrderedData, 0,
               ProducerClosure::kPendingIngress);
}

TEST_F(Pm4SdmaStreamingTest, ProducerClosureWithUnusedInput) {
  RunStreaming(kStressWorkload, AcquireMode::kFullBarrier, 0,
               ProducerClosure::kUnusedInput);
}

TEST_F(Pm4SdmaStreamingTest, OrderedDataAcquireProducerClosureWithUnusedInput) {
  RunStreaming(kStressWorkload, AcquireMode::kOrderedData, 0,
               ProducerClosure::kUnusedInput);
}

}  // namespace
