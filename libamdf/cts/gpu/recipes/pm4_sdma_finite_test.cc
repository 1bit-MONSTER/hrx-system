// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "libamdf/cts/gpu/recipes/pm4_sdma_finite_fixture.h"

namespace {

constexpr FiniteStreamWorkload kPresubmitWorkload = {
    .graph_count = 16,
    .streams_per_credit_count = 2,
    .require_native_ring_wrap = false,
};

TEST_F(Pm4SdmaFiniteStreamTest, SeparateTransfersRetireEveryGraph) {
  RunCoherentStreams(TransferQueues::kSeparate, kPresubmitWorkload);
}

TEST_F(Pm4SdmaFiniteStreamTest, SharedPairedTransfersRetireEveryGraph) {
  RunCoherentStreams(TransferQueues::kSharedPaired, kPresubmitWorkload);
}

TEST_F(Pm4SdmaFiniteStreamTest, SharedCreditGroupedTransfersRetireEveryGraph) {
  RunCoherentStreams(TransferQueues::kSharedCreditGrouped, kPresubmitWorkload);
}

}  // namespace
