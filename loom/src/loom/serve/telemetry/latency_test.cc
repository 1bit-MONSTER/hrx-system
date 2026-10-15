// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/serve/telemetry/latency.h"

#include <initializer_list>

#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"
#include "loom/serve/telemetry/record.h"
#include "loom/serve/telemetry/stream.h"

namespace {

TEST(TelemetryLatency, InclusiveBoundsAndOverflowAreDisjoint) {
  for (size_t outcome = 0; outcome <= LOOM_SERVE_TELEMETRY_FAILED; ++outcome) {
    loom_serve_telemetry_latency_t latency = {};
    loom_serve_telemetry_latency_observe(&latency.outcomes[outcome], 0);
    uint64_t sum_ns = 0;
    for (size_t i = 0; i + 1 < LOOM_SERVE_TELEMETRY_LATENCY_BUCKET_COUNT; ++i) {
      const uint64_t bound = loom_serve_telemetry_latency_bound_ns(i);
      for (uint64_t duration : {bound - 1, bound, bound + 1}) {
        loom_serve_telemetry_latency_observe(&latency.outcomes[outcome],
                                             duration);
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
    loom_serve_telemetry_latency_observe(&source.outcomes[i], 123);
    loom_serve_telemetry_latency_observe(&source.outcomes[i], 1000001);
    loom_serve_telemetry_latency_observe(&source.first_output, 2500001);
    loom_serve_telemetry_latency_merge(&accumulated, &source);
    loom_serve_telemetry_latency_merge(&accumulated, &source);
    EXPECT_EQ(source.outcomes[i].sum_ns, 1000124u);
    EXPECT_EQ(source.first_output.sum_ns, 2500001u);
  }
  for (const auto& histogram : accumulated.outcomes) {
    EXPECT_EQ(histogram.sum_ns, 2000248u);
    EXPECT_EQ(histogram.buckets[0], 2u);
    EXPECT_EQ(histogram.buckets[1], 2u);
    for (size_t j = 2; j < LOOM_SERVE_TELEMETRY_LATENCY_BUCKET_COUNT; ++j) {
      EXPECT_EQ(histogram.buckets[j], 0u);
    }
  }
  EXPECT_EQ(accumulated.first_output.buckets[2], 8u);
  EXPECT_EQ(accumulated.first_output.sum_ns, 20000008u);
}

TEST(TelemetryLatency, FirstOutputEventAndHistogramShareOneObservation) {
  loom_serve_telemetry_stream_t* stream = nullptr;
  IREE_ASSERT_OK(loom_serve_telemetry_stream_allocate(
      2, iree_allocator_system(), &stream));
  loom_serve_telemetry_latency_t latency = {};
  const iree_time_t arrival_time = iree_time_now();
  loom_serve_telemetry_latency_publish_output(&latency, stream, 37,
                                              arrival_time);
  uint64_t cursor = 0;
  loom_serve_telemetry_record_t record;
  ASSERT_EQ(loom_serve_telemetry_stream_read(stream, &cursor, &record),
            LOOM_SERVE_TELEMETRY_READ_RECORD);
  loom_serve_telemetry_stream_free(stream);
  EXPECT_EQ(record.event, &LOOM_SERVE_TELEMETRY_FIRST_OUTPUT);
  EXPECT_EQ(record.request, 37u);
  EXPECT_EQ(record.time_ns - arrival_time,
            record.data.first_output.duration_ns);
  EXPECT_EQ(latency.first_output.sum_ns, record.data.first_output.duration_ns);
  uint64_t count = 0;
  for (const auto bucket : latency.first_output.buckets) {
    count += bucket;
  }
  EXPECT_EQ(count, 1u);
  for (const auto& histogram : latency.outcomes) {
    EXPECT_EQ(histogram.sum_ns, 0u);
    for (const auto bucket : histogram.buckets) {
      EXPECT_EQ(bucket, 0u);
    }
  }
}

}  // namespace
