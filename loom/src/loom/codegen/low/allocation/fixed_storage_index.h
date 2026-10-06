// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Immutable exact storage claims for resolved fixed-value assignments.

#ifndef LOOM_CODEGEN_LOW_ALLOCATION_FIXED_STORAGE_INDEX_H_
#define LOOM_CODEGEN_LOW_ALLOCATION_FIXED_STORAGE_INDEX_H_

#include "iree/base/api.h"
#include "iree/base/internal/arena.h"
#include "loom/ir/ir.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct loom_low_allocation_fixed_storage_record_t
    loom_low_allocation_fixed_storage_record_t;
typedef struct loom_low_allocation_fixed_location_entry_t
    loom_low_allocation_fixed_location_entry_t;
typedef struct loom_low_allocation_fixed_location_group_t
    loom_low_allocation_fixed_location_group_t;

struct loom_low_allocation_target_constraints_t;
struct loom_low_allocation_assignment_t;
struct loom_low_allocation_unit_liveness_t;

// Storage-partitioned temporal index over immutable fixed assignments.
//
// Each record is one canonical atomic-storage and exact live-segment claim.
// Records are grouped by location kind and storage identity, with an implicit
// maximum-end interval tree inside each identity group. Query generations
// exclude the candidate and explicitly ignored required storage components
// without scanning the ignored set for every matching claim.
typedef struct loom_low_allocation_fixed_storage_index_t {
  // Exact claims ordered by storage identity then start point.
  loom_low_allocation_fixed_storage_record_t* records;
  // Tied root shared by every implicit record subtree, INVALID for mixed
  // subtrees, or NULL when no fixed assignment belongs to a shared component.
  loom_value_ordinal_t* subtree_tied_roots;
  // First storage record for each register-like location kind.
  uint32_t record_starts[2];
  // Number of storage records for each register-like location kind.
  uint32_t record_counts[2];
  // Last query generation excluding each component, indexed by its origin's
  // resolved fixed-value index.
  uint32_t* excluded_generations;
  // Current nonzero query generation.
  uint32_t generation;
} loom_low_allocation_fixed_storage_index_t;

// Attempt-local ordered availability projected from immutable fixed claims.
//
// Allocation visits acquisition starts monotonically. The cursor advances each
// location to its first non-expired claim and retains the earliest point at
// which that location becomes unavailable. Balanced location subtrees retain
// the maximum such point, allowing a search to skip a dense run when every
// location conflicts with the candidate lifetime. The state is rebuilt for an
// independent allocation attempt and never escapes its scratch arena.
typedef struct loom_low_allocation_fixed_availability_t {
  // Immutable target constraints supplying exact fixed records.
  const struct loom_low_allocation_target_constraints_t* constraints;
  // Ordered unique fixed locations and mutable claim cursors.
  loom_low_allocation_fixed_location_entry_t* entries;
  // Storage-identity partitions over contiguous spans of |entries|.
  loom_low_allocation_fixed_location_group_t* groups;
  // Number of initialized groups in |groups|.
  uint32_t group_count;
  // Min-heap of entry indices ordered by current claim end point.
  uint32_t* expiration_heap;
  // Number of live entries in |expiration_heap|.
  uint32_t expiration_heap_count;
  // Latest acquisition start projected into the mutable cursors.
  uint32_t start_point;
} loom_low_allocation_fixed_availability_t;

// Builds the fixed-storage index owned by |constraints|. Retained storage
// comes from |arena|. Construction is linear in materialized exact claims plus
// bounded radix passes and returns temporary sorting blocks before completion.
iree_status_t loom_low_allocation_fixed_storage_index_initialize(
    struct loom_low_allocation_target_constraints_t* constraints,
    const struct loom_low_allocation_unit_liveness_t* unit_liveness,
    iree_arena_allocator_t* arena);

// Returns true when |candidate| violates its own fixed binding or conflicts
// with another fixed value or implicit physical write. Resolved whole-value
// tied components share reservations when their concrete storage matches;
// they never excuse clobbers.
bool loom_low_allocation_target_constraints_fixed_storage_conflicts(
    struct loom_low_allocation_target_constraints_t* constraints,
    const struct loom_low_allocation_unit_liveness_t* unit_liveness,
    const struct loom_low_allocation_assignment_t* candidate,
    const loom_value_id_t* ignored_value_ids, uint16_t ignored_value_count);

// Initializes an attempt-local ordered view over |constraints|' immutable fixed
// claims. All storage belongs to |arena| and requires no deinitialization.
iree_status_t loom_low_allocation_fixed_availability_initialize(
    const struct loom_low_allocation_target_constraints_t* constraints,
    iree_arena_allocator_t* arena,
    loom_low_allocation_fixed_availability_t* out_availability);

// Returns true when |availability| can provide ordered availability for
// |candidate|. Eligible candidates have one continuously live linear unit and
// are not themselves fixed to a location.
bool loom_low_allocation_fixed_availability_can_order_candidate(
    const loom_low_allocation_fixed_availability_t* availability,
    const struct loom_low_allocation_assignment_t* candidate);

// Finds the first location at or after |minimum_base| whose fixed claims do not
// overlap |candidate|, bounded by |maximum_base|. The candidate must be a
// continuous scalar in a linear register class and must not itself be fixed.
// Calls on one availability state use nondecreasing candidate start points.
bool loom_low_allocation_fixed_availability_find_next_location(
    loom_low_allocation_fixed_availability_t* availability,
    const struct loom_low_allocation_assignment_t* candidate,
    uint32_t minimum_base, uint32_t maximum_base, uint32_t* out_base);

// Finds the last location at or before |maximum_base| whose fixed claims do not
// overlap |candidate|, bounded by |minimum_base|. Preconditions match the
// forward query above.
bool loom_low_allocation_fixed_availability_find_previous_location(
    loom_low_allocation_fixed_availability_t* availability,
    const struct loom_low_allocation_assignment_t* candidate,
    uint32_t minimum_base, uint32_t maximum_base, uint32_t* out_base);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_CODEGEN_LOW_ALLOCATION_FIXED_STORAGE_INDEX_H_
