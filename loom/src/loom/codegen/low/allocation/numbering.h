// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Footprint-preserving physical register numbering of a completed allocation.

#ifndef LOOM_CODEGEN_LOW_ALLOCATION_NUMBERING_H_
#define LOOM_CODEGEN_LOW_ALLOCATION_NUMBERING_H_

#include "iree/base/api.h"
#include "iree/base/internal/arena.h"
#include "loom/codegen/low/allocation/interval_assignment.h"
#include "loom/codegen/low/allocation/move.h"
#include "loom/codegen/low/allocation/storage_lease.h"
#include "loom/codegen/low/allocation/target_constraints.h"
#include "loom/codegen/low/allocation/unit_liveness.h"
#include "loom/codegen/low/placement.h"

#ifdef __cplusplus
extern "C" {
#endif

// Mutable allocation facts before publishing the final table. Storage
// assignment and move sequencing are complete; numbering changes only
// physical coordinates, never interference, move topology or resource bounds.
typedef struct loom_low_allocation_numbering_context_t {
  // Producer-retained operand requirements and hard storage relations.
  const loom_low_placement_table_t* placement;
  // Bound instruction and scheduled-pair preferences.
  const loom_low_placement_preference_index_t* preferences;
  // Original class extents and fixed/reserved locations, unchanged by
  // numbering.
  const loom_low_allocation_target_constraints_t* target_constraints;
  // External source coordinates indexed by formal-argument ordinal. Numbering
  // preserves these even when entry transport was an elided identity.
  const loom_low_allocation_abi_location_t* entry_locations;
  // Number of entries in |entry_locations|.
  iree_host_size_t entry_location_count;
  // Retained implicit physical uses anchoring architectural locations.
  const loom_low_allocation_unit_liveness_t* unit_liveness;
  // Completed assignments and ordinal lookup, updated in place.
  loom_low_allocation_interval_assignment_result_t* interval_assignment;
  // Completed leases, whose physical index is rebuilt in its existing storage.
  loom_low_allocation_storage_lease_state_t* storage_leases;
  // Final physical moves, including cycle-scratch endpoints.
  loom_low_move_t* moves;
  // Number of initialized physical move rows.
  iree_host_size_t move_count;
} loom_low_allocation_numbering_context_t;

// Improves retained instruction costs with a bounded search over bijections
// within the existing physical extent. Every assignment/lease remains rigid,
// every class stays within its original extent, and no preference exceeds its
// original cost. Explicit physical-register views and target address-state
// locations retain their original numbering. No instruction uses means no
// work or scratch allocation. Only scratch allocation can fail.
iree_status_t loom_low_allocation_number_registers(
    const loom_low_allocation_numbering_context_t* context,
    iree_arena_allocator_t* scratch_arena);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_CODEGEN_LOW_ALLOCATION_NUMBERING_H_
