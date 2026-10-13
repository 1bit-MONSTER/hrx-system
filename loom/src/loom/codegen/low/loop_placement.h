// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Reversible loop-local placement over retained schedule and allocation facts.

#ifndef LOOM_CODEGEN_LOW_LOOP_PLACEMENT_H_
#define LOOM_CODEGEN_LOW_LOOP_PLACEMENT_H_

#include "loom/codegen/low/allocation.h"
#include "loom/codegen/low/loop_execution.h"
#include "loom/codegen/low/schedule/types.h"

#ifdef __cplusplus
extern "C" {
#endif

// Target cost envelope for one descriptor eligible for loop-local placement.
typedef struct loom_low_loop_placement_cost_t {
  // Inclusive maximum execution multiplier admitted for the moved chain.
  uint64_t maximum_execution_multiplier;
  // Maximum schedule-extent cycles contributed by the complete moved chain.
  uint16_t maximum_schedule_cycle_count;
} loom_low_loop_placement_cost_t;

// Queries the target cost envelope for one immutable descriptor. False leaves
// the descriptor in its original block. Accepted descriptors must have a
// concrete native realization accounted for by the returned schedule costs.
typedef bool (*loom_low_loop_placement_cost_query_fn_t)(
    void* user_data, const loom_low_descriptor_t* descriptor,
    loom_low_loop_placement_cost_t* out_cost);

// One private destructive definition proposed for placement at its sole use.
typedef struct loom_low_loop_placement_candidate_t {
  // Transparent copy establishing the descriptor's tied result storage.
  loom_op_t* copy_op;
  // Target-priced descriptor operation producing the spilled value.
  loom_op_t* descriptor_op;
  // Sole result user before which both operations are placed.
  loom_op_t* destination_op;
  // Original operation following descriptor_op, retained for exact rollback.
  loom_op_t* source_anchor_op;
  // Baseline schedule node containing |copy_op|.
  uint32_t source_node_index;
  // Baseline schedule node containing |destination_op|.
  uint32_t destination_node_index;
  // Maximum schedule-extent cycles contributed by the complete moved chain.
  uint16_t maximum_schedule_cycle_count;
} loom_low_loop_placement_candidate_t;

static_assert(sizeof(loom_low_loop_placement_candidate_t) <= 48,
              "loop placement candidates must remain compact");

// Arena-owned proposal borrowing one immutable baseline frame. Operations may
// move while the proposal is live; rejection restores their exact source order.
typedef struct loom_low_loop_placement_plan_t {
  // Candidates in original source order.
  loom_low_loop_placement_candidate_t* candidates;
  // Number of entries in |candidates|.
  iree_host_size_t candidate_count;
  // Sorted value IDs spilled by the baseline allocation. Trial allocations
  // may remove entries but cannot introduce a different spilled value.
  loom_value_id_t* baseline_spill_value_ids;
  // Number of entries in |baseline_spill_value_ids|.
  iree_host_size_t baseline_spill_value_count;
  // Maximum admitted schedule-extent growth indexed by destination block.
  uint32_t* schedule_cycle_allowances_by_block;
  // Blocks whose instructions and local dependencies change under the plan.
  iree_bitmap_t changed_blocks;
} loom_low_loop_placement_plan_t;

// Proposes private definitions for placement at their sole loop-local use from
// retained control-flow, placement, use, and spill facts. Planning does not
// mutate the function.
iree_status_t loom_low_loop_placement_plan(
    const loom_low_schedule_table_t* schedule,
    const loom_low_allocation_table_t* allocation,
    const loom_low_loop_execution_map_t* loop_execution,
    loom_low_loop_placement_cost_query_fn_t cost_query,
    void* cost_query_user_data, iree_arena_allocator_t* arena,
    loom_low_loop_placement_plan_t* out_plan);

// Moves every proposed copy and descriptor before its sole result user.
iree_status_t loom_low_loop_placement_apply(
    loom_rewriter_t* rewriter, const loom_low_loop_placement_plan_t* plan);

// Restores every proposed copy and descriptor in exact original source order.
iree_status_t loom_low_loop_placement_rollback(
    loom_rewriter_t* rewriter, const loom_low_loop_placement_plan_t* plan);

// Returns true when the trial makes every moved chain register-resident,
// eliminates spill storage, and pays for all newly loop-local native work
// without increasing spill, store, or move costs.
bool loom_low_loop_placement_improves_allocation(
    const loom_low_loop_placement_plan_t* plan,
    const loom_low_allocation_table_t* baseline,
    const loom_low_schedule_table_t* trial_schedule,
    const loom_low_allocation_table_t* trial_allocation);

// Returns true when modeled block-extent growth is bounded by the exact native
// work introduced by the proposal. Allocation economics are checked separately.
bool loom_low_loop_placement_bounds_schedule(
    const loom_low_loop_placement_plan_t* plan,
    const loom_low_schedule_table_t* baseline,
    const loom_low_schedule_table_t* trial);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_CODEGEN_LOW_LOOP_PLACEMENT_H_
