// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/serve/telemetry/latency.h"

#include <initializer_list>

#include "iree/testing/gtest.h"

namespace {

TEST(TelemetryLatency, InclusiveBoundsAndOverflowAreDisjoint) {
  for (size_t outcome = 0; outcome <= LOOM_SERVE_TELEMETRY_FAILED; ++outcome) {
    loom_serve_telemetry_latency_t latency = {};
    const auto kind = static_cast<loom_serve_telemetry_outcome_t>(outcome);
    loom_serve_telemetry_latency_observe(&latency, kind, 0);
    uint64_t sum_ns = 0;
    for (size_t i = 0; i + 1 < LOOM_SERVE_TELEMETRY_LATENCY_BUCKET_COUNT; ++i) {
      const uint64_t bound = loom_serve_telemetry_latency_bound_ns(i);
      for (uint64_t duration : {bound - 1, bound, bound + 1}) {
        loom_serve_telemetry_latency_observe(&latency, kind, duration);
        sum_ns += duration;
      }
    }
    for (size_t i = 0; i < IREE_ARRAYSIZE(latency.outcomes); ++i) {
      const auto& histogram = latency.outcomes[i];
      EXPECT_EQ(histogram.sum_ns, i == outcome ? sum_ns : 0u);
      for (size_t j = 0; j < LOOM_SERVE_TELEMETRY_LATENCY_BUCKET_COUNT; ++j) {
        const uint64_t expected =
            i != outcome                                         ? 0
            : j + 1 == LOOM_SERVE_TELEMETRY_LATENCY_BUCKET_COUNT ? 1
                                                                 : 3;
        EXPECT_EQ(histogram.buckets[j], expected) << i << ":" << j;
      }
    }
  }
}

TEST(TelemetryLatency, MergePreservesIndependentOutcomesAndExactSums) {
  loom_serve_telemetry_latency_t accumulated = {};
  for (size_t i = 0; i <= LOOM_SERVE_TELEMETRY_FAILED; ++i) {
    loom_serve_telemetry_latency_t source = {};
    const auto outcome = static_cast<loom_serve_telemetry_outcome_t>(i);
    loom_serve_telemetry_latency_observe(&source, outcome, 123);
    loom_serve_telemetry_latency_observe(&source, outcome, 1000001);
    loom_serve_telemetry_latency_merge(&accumulated, &source);
    loom_serve_telemetry_latency_merge(&accumulated, &source);
    EXPECT_EQ(source.outcomes[i].sum_ns, 1000124u);
  }
  for (const auto& histogram : accumulated.outcomes) {
    EXPECT_EQ(histogram.sum_ns, 2000248u);
    EXPECT_EQ(histogram.buckets[0], 2u);
    EXPECT_EQ(histogram.buckets[1], 2u);
    for (size_t j = 2; j < LOOM_SERVE_TELEMETRY_LATENCY_BUCKET_COUNT; ++j) {
      EXPECT_EQ(histogram.buckets[j], 0u);
    }
  }
}

}  // namespace
