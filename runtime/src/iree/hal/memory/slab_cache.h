// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef IREE_HAL_MEMORY_SLAB_CACHE_H_
#define IREE_HAL_MEMORY_SLAB_CACHE_H_

#include "iree/base/api.h"
#include "iree/hal/pool.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

// Explicit retention and preparation policy for one reusable slab class.
typedef struct iree_hal_slab_cache_options_t {
  // Geometry acquired from the backing pool. Compatible requests receive the
  // entire entry; larger or more aligned requests use the same backing pool
  // without entering the idle cache.
  iree_hal_pool_reservation_request_t slab;
  // Ready idle entries proactively prepared on the captured memory owner.
  // Zero disables prefill. Pending entries also consume the idle limit.
  uint32_t target_count;
  // Maximum idle entries retained, including pending reuse prerequisites.
  // Live reservations do not count against this limit. Zero retains none.
  uint32_t max_count;
  // Copied diagnostic name for logical reservations.
  iree_string_view_t trace_name;
} iree_hal_slab_cache_options_t;

// Selects target_count=0, max_count=4 and an unset slab class.
void iree_hal_slab_cache_options_initialize(
    iree_hal_slab_cache_options_t* out_options);

// Retains |backing_pool| once and captures its native maintenance owner.
// Entries own ordinary reservations, prepared views and exact reuse history.
// Hot hits and returns neither allocate metadata nor call the backing pool.
// Growth-enabled misses join cold preparation on the captured owner, without
// waiting for another allocation to release capacity. No-growth misses report
// EXHAUSTED/GROWTH_REQUIRED. Refill errors propagate on the next acquisition.
//
// Prefault touches only actually retired storage. A pending parent prerequisite
// is preserved without touching its bytes; ordinary reservation eligibility
// still permits an ordered borrower to accept that range with a dependency.
// The cache and its backing pool must outlive all reservations and views.
// Trim also schedules an ordered sweep after previously queued child returns;
// it never joins native preparation on the caller. Committed-byte statistics
// exclude detached returns even while their native retirement is pending.
// Final destruction joins this cache's maintenance and runs outside the
// captured executor after the caller has retired its execution.
iree_status_t iree_hal_slab_cache_create(
    iree_hal_pool_t* backing_pool, const iree_hal_slab_cache_options_t* options,
    iree_allocator_t host_allocator, iree_hal_pool_t** out_pool);

// Changes the proactive target, which must not exceed max_count. Setting zero
// stops further refill admission; it does not itself trim retained entries.
iree_status_t iree_hal_slab_cache_set_target(iree_hal_pool_t* cache,
                                             uint32_t target_count);

typedef struct iree_hal_slab_cache_stats_t {
  // Idle entries whose prior uses have actually completed.
  uint64_t ready_count;
  // Idle entries with outstanding reuse prerequisites.
  uint64_t pending_count;
  // Requests satisfied from previously prepared idle entries.
  uint64_t hit_count;
  // Compatible requests requiring cold growth.
  uint64_t miss_count;
  // Requests outside the cache's size/alignment class.
  uint64_t bypass_count;
  // Exponentially weighted observed reuse interval in nanoseconds.
  uint64_t ema_reuse_interval_nanoseconds;
  // Cumulative time spent prefaulting retired backing in nanoseconds.
  uint64_t prefault_time_nanoseconds;
} iree_hal_slab_cache_stats_t;

// Cold snapshot of cache behavior. Counts distinguish actual completion from
// requester dominance. The common pool query remains an O(1) atomic snapshot.
iree_status_t iree_hal_slab_cache_query_stats(
    const iree_hal_pool_t* cache, iree_hal_slab_cache_stats_t* out_stats);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // IREE_HAL_MEMORY_SLAB_CACHE_H_
