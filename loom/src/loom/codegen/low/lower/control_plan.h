// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Planned Low blocks and source-edge interpositions. Block identities exist
// before any Low IR and remain stable when emitted blocks are inserted or
// moved.

#ifndef LOOM_CODEGEN_LOW_LOWER_CONTROL_PLAN_H_
#define LOOM_CODEGEN_LOW_LOWER_CONTROL_PLAN_H_

#include "loom/codegen/low/lower/lower.h"

#ifdef __cplusplus
extern "C" {
#endif

// One-based identity in the function's planned block domain. Zero denotes an
// absent block, an empty signature, or tail placement as documented below.
typedef uint32_t loom_low_lower_block_ref_t;

typedef struct loom_low_lower_control_plan_t loom_low_lower_control_plan_t;

// Plans target branch expansion and branch/return operand conversions after
// source operation selection has finalized producer and boundary signatures.
// Construction scratch is released before returning. No Low blocks, values,
// or symbols are published by this phase.
iree_status_t loom_low_lower_control_plan_build(
    loom_low_lower_context_t* context);

// Number of authored and synthetic blocks addressed by the complete plan.
iree_host_size_t loom_low_lower_control_block_count(
    const loom_low_lower_context_t* context);

// Returns the stable identity of an authored top-level CFG block.
loom_low_lower_block_ref_t loom_low_lower_control_source_block(
    const loom_block_t* source_block);

// Plans one synthetic block. A nonzero |before| inserts it immediately before
// that block; zero appends it. |signature| names an already planned block whose
// complete argument tuple is copied, including supplemental carried values.
// Zero selects an empty tuple. Neither operation constructs IR.
iree_status_t loom_low_lower_control_add_block(
    loom_low_lower_context_t* context, loom_low_lower_block_ref_t before,
    loom_low_lower_block_ref_t signature,
    loom_low_lower_block_ref_t* out_block);

// Returns the planned argument count and type without constructing SSA values.
uint16_t loom_low_lower_control_argument_count(
    const loom_low_lower_context_t* context, loom_low_lower_block_ref_t block);
loom_type_t loom_low_lower_control_argument_type(
    const loom_low_lower_context_t* context, loom_low_lower_block_ref_t block,
    uint16_t argument_index);

// Returns the effective planned destination of an authored successor.
loom_low_lower_block_ref_t loom_low_lower_control_successor(
    const loom_low_lower_context_t* context, const loom_op_t* source_terminator,
    uint8_t successor_index);

// Interposes a planned block with the same payload on an authored edge. Returns
// the previous destination so nested expansions preserve their continuations.
iree_status_t loom_low_lower_control_interpose_successor(
    loom_low_lower_context_t* context, const loom_op_t* source_terminator,
    uint8_t successor_index, loom_low_lower_block_ref_t block,
    loom_low_lower_block_ref_t* out_previous);

// Records and retrieves a target's branch recipe at its source block ordinal.
void loom_low_lower_set_branch_plan(loom_low_lower_context_t* context,
                                    const loom_op_t* source_terminator,
                                    loom_low_lower_plan_t plan);
bool loom_low_lower_lookup_branch_plan(loom_low_lower_context_t* context,
                                       const loom_op_t* source_terminator,
                                       loom_low_lower_plan_t* out_plan);

// Emits the conditional branch corresponding to an authored source
// terminator and binds any producer-proven loop trip count to the emitted Low
// control. Target branch expansion calls this only for the final branch
// representing the source terminator; target-internal control branches use
// low.cond_br directly.
iree_status_t loom_low_lower_control_emit_cond_branch(
    loom_low_lower_context_t* context, const loom_op_t* source_terminator,
    loom_value_id_t low_condition, loom_block_t* low_true_dest,
    loom_block_t* low_false_dest);

// Applies a prepared branch or callable-exit operand conversion. The operand
// ordinal addresses the terminator's source tuple; |required_type| is the
// retained receiving signature. Matching carriers need no target callback.
iree_status_t loom_low_lower_control_materialize_operand(
    loom_low_lower_context_t* context, const loom_op_t* source_terminator,
    uint16_t operand_index, loom_type_t required_type,
    loom_value_id_t* inout_low_value);

// Creates the retained synthetic blocks after all authored Low blocks and
// their complete argument tuples have been constructed. Only allocation can
// fail; topology and signature decisions were completed during planning.
iree_status_t loom_low_lower_control_create_blocks(
    loom_low_lower_context_t* context);

// Resolves a planned identity after block construction. Zero returns NULL.
loom_block_t* loom_low_lower_control_block(
    const loom_low_lower_context_t* context, loom_low_lower_block_ref_t block);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_CODEGEN_LOW_LOWER_CONTROL_PLAN_H_
