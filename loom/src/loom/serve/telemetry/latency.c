// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/serve/telemetry/latency.h"

static const uint64_t loom_serve_telemetry_latency_bounds_ns[] = {
    1000000,      2500000,      5000000,      10000000,      25000000,
    50000000,     100000000,    250000000,    500000000,     1000000000,
    2500000000,   5000000000,   10000000000,  25000000000,   50000000000,
    100000000000, 250000000000, 500000000000, 1000000000000,
};
static_assert(IREE_ARRAYSIZE(loom_serve_telemetry_latency_bounds_ns) + 1 ==
                  LOOM_SERVE_TELEMETRY_LATENCY_BUCKET_COUNT,
              "finite bounds plus the unbounded bucket");

uint64_t loom_serve_telemetry_latency_bound_ns(iree_host_size_t index) {
  return loom_serve_telemetry_latency_bounds_ns[index];
}

void loom_serve_telemetry_latency_observe(
    loom_serve_telemetry_latency_t* latency,
    loom_serve_telemetry_outcome_t outcome, uint64_t duration_ns) {
  iree_host_size_t first = 0;
  iree_host_size_t last =
      IREE_ARRAYSIZE(loom_serve_telemetry_latency_bounds_ns);
  while (first < last) {
    const iree_host_size_t middle = first + (last - first) / 2;
    if (duration_ns <= loom_serve_telemetry_latency_bounds_ns[middle]) {
      last = middle;
    } else {
      first = middle + 1;
    }
  }
  loom_serve_telemetry_latency_histogram_t* histogram =
      &latency->outcomes[outcome];
  ++histogram->buckets[first];
  histogram->sum_ns += duration_ns;
}

void loom_serve_telemetry_latency_merge(
    loom_serve_telemetry_latency_t* target,
    const loom_serve_telemetry_latency_t* source) {
  for (iree_host_size_t i = 0; i < IREE_ARRAYSIZE(target->outcomes); ++i) {
    for (iree_host_size_t j = 0; j < LOOM_SERVE_TELEMETRY_LATENCY_BUCKET_COUNT;
         ++j) {
      target->outcomes[i].buckets[j] += source->outcomes[i].buckets[j];
    }
    target->outcomes[i].sum_ns += source->outcomes[i].sum_ns;
  }
}
