// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_SERVE_TELEMETRY_LATENCY_H_
#define LOOM_SERVE_TELEMETRY_LATENCY_H_

#include "loom/serve/telemetry/record.h"

#ifdef __cplusplus
extern "C" {
#endif

enum { LOOM_SERVE_TELEMETRY_LATENCY_BUCKET_COUNT = 20 };

// Single-owner duration distribution. All-zero storage is an empty histogram.
// Disjoint buckets keep each observation to one increment; readers form
// cumulative counts without modifying the producer's storage.
typedef struct loom_serve_telemetry_latency_histogram_t {
  // Counts for increasing inclusive upper bounds; the last bucket is unbounded.
  uint64_t buckets[LOOM_SERVE_TELEMETRY_LATENCY_BUCKET_COUNT];
  // Sum of all observed durations in nanoseconds, independent of bucketing.
  uint64_t sum_ns;
} loom_serve_telemetry_latency_histogram_t;

// Model protocol receipt through terminal owner retirement, not peer receipt or
// device time. Stored separately from compact work counters and event records.
// Updated and copied only by the serialized serving owner, without allocation,
// locking, formatting, or consulting telemetry readers.
typedef struct loom_serve_telemetry_latency_t {
  // Exactly one duration per terminal request, indexed by its existing outcome.
  loom_serve_telemetry_latency_histogram_t
      outcomes[LOOM_SERVE_TELEMETRY_FAILED + 1];
} loom_serve_telemetry_latency_t;

// Returns the inclusive upper bound in nanoseconds for a finite bucket. The
// caller supplies an index below LATENCY_BUCKET_COUNT - 1; the final unbounded
// bucket has no numeric bound.
uint64_t loom_serve_telemetry_latency_bound_ns(iree_host_size_t index);

// Observes one nonnegative duration at the same ownership boundary that counts
// the terminal outcome. The caller supplies trusted request lifetime state.
void loom_serve_telemetry_latency_observe(
    loom_serve_telemetry_latency_t* latency,
    loom_serve_telemetry_outcome_t outcome, uint64_t duration_ns);

// Adds a complete owner snapshot, including buckets and exact sums. Source and
// target are distinct; neither is being concurrently updated.
void loom_serve_telemetry_latency_merge(
    loom_serve_telemetry_latency_t* target,
    const loom_serve_telemetry_latency_t* source);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_SERVE_TELEMETRY_LATENCY_H_
