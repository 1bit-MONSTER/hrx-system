// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Immutable point queries over final assignment storage lifetimes.

#ifndef LOOM_CODEGEN_LOW_ALLOCATION_STORAGE_LIVENESS_INDEX_H_
#define LOOM_CODEGEN_LOW_ALLOCATION_STORAGE_LIVENESS_INDEX_H_

#include "iree/base/api.h"
#include "iree/base/internal/arena.h"
#include "loom/codegen/low/allocation/assignment.h"
#include "loom/codegen/low/allocation/move.h"
#include "loom/codegen/low/allocation/unit_liveness.h"
#include "loom/codegen/low/descriptors.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct loom_low_allocation_storage_lifetime_t
    loom_low_allocation_storage_lifetime_t;

// Canonical storage-unit lifetimes sorted by storage identity and start point.
//
// Construction materializes each refined logical assignment unit into its
// target-visible atomic storage units. One binary search answers each atomic
// point query.
typedef struct loom_low_allocation_storage_liveness_index_t {
  // Descriptor set whose storage alias contract defines this index.
  const loom_low_descriptor_set_t* descriptor_set;
  // Lifetimes grouped by location kind and sorted by storage identity and
  // start point.
  loom_low_allocation_storage_lifetime_t* lifetimes;
  // First lifetime record for each register-like location kind.
  uint32_t lifetime_starts[2];
  // Number of lifetime records for each register-like location kind.
  uint32_t lifetime_counts[2];
} loom_low_allocation_storage_liveness_index_t;

// Builds an immutable index over the refined per-unit lifetimes of final
// register-like |assignments|. Retained storage comes from |arena| and may be
// released after the last query. Construction uses a nested arena over the
// same block pool and returns those blocks before completion.
iree_status_t loom_low_allocation_storage_liveness_index_initialize(
    const loom_low_descriptor_set_t* descriptor_set,
    const loom_low_allocation_assignment_t* assignments,
    iree_host_size_t assignment_count,
    const loom_low_allocation_unit_liveness_t* unit_liveness,
    iree_arena_allocator_t* arena,
    loom_low_allocation_storage_liveness_index_t* out_index);

// Returns true when register-like |location| is occupied at |point|. The
// location must obey the same descriptor-set storage contract used to
// construct |index|.
bool loom_low_allocation_storage_liveness_index_is_live_at_point(
    const loom_low_allocation_storage_liveness_index_t* index,
    const loom_low_move_location_t* location, uint32_t point);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_CODEGEN_LOW_ALLOCATION_STORAGE_LIVENESS_INDEX_H_
