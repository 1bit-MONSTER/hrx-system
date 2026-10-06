// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// SCF simplification and source-combination rewrites.

#ifndef LOOM_OPS_SCF_CANONICALIZE_H_
#define LOOM_OPS_SCF_CANONICALIZE_H_

#include "loom/ops/op_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

// Combines an ordered integer comparison selecting its own operands into
// min/max. Only run before target legalization: targets without native extrema
// decompose them into the same comparison and selection.
iree_status_t loom_scf_select_combine_integer_extremum(
    loom_op_t* op, loom_rewriter_t* rewriter, bool* out_changed);

// Returns the yield of a complete single-block region, or NULL when the region
// has not reached a shape that canonicalization can rewrite.
loom_op_t* loom_scf_region_terminator(loom_region_t* region);

// Moves the operations before |terminator| into the enclosing block, preserving
// their order, values, effects, and nested region ownership.
iree_status_t loom_scf_move_region_body_before_op(loom_rewriter_t* rewriter,
                                                  loom_region_t* region,
                                                  loom_op_t* terminator,
                                                  loom_op_t* before_op);

// Replaces the results and erases |op| when its result tuple is complete.
iree_status_t loom_scf_replace_results_and_erase(
    loom_op_t* op, loom_rewriter_t* rewriter,
    const loom_value_id_t* replacements, uint16_t replacement_count);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_OPS_SCF_CANONICALIZE_H_
