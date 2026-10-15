// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "libamdf/cts/gpu/recipes/pm4_sdma_finite_fixture.h"

namespace {

constexpr FiniteStreamWorkload kStressWorkload = {
    .graph_count = 64,
    .streams_per_credit_count = 5,
    .require_native_ring_wrap = true,
};

TEST_F(Pm4SdmaFiniteStreamTest, SeparateTransfersTraverseNativeRings) {
  RunCoherentStreams(TransferQueues::kSeparate, kStressWorkload);
}

TEST_F(Pm4SdmaFiniteStreamTest, SharedPairedTransfersTraverseNativeRings) {
  RunCoherentStreams(TransferQueues::kSharedPaired, kStressWorkload);
}

TEST_F(Pm4SdmaFiniteStreamTest,
       SharedCreditGroupedTransfersTraverseNativeRings) {
  RunCoherentStreams(TransferQueues::kSharedCreditGrouped, kStressWorkload);
}

}  // namespace
