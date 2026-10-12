// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Generic vector reference legalizers.

#ifndef LOOM_TRANSFORMS_VECTOR_TARGET_LEGALIZATION_H_
#define LOOM_TRANSFORMS_VECTOR_TARGET_LEGALIZATION_H_

#include "loom/ops/vector/ops.h"
#include "loom/target/legalization.h"

#ifdef __cplusplus
extern "C" {
#endif

// Rewrites a static vector.from_elements as a rank-one splat/insert chain.
// Multidimensional results are restored with a shape-only vector.bitcast, so
// callers must know that the selected target accepts vector shape aliases.
iree_status_t loom_vector_from_elements_linearize_rewrite_op(
    loom_rewriter_t* rewriter, loom_op_t* op, bool* out_rewritten);

// Rewrites a static vector.transpose as a row-major rank-one vector.shuffle,
// surrounded by shape-only vector.bitcast operations when needed. Static
// transposes with an identity lane permutation become a shape-only bitcast.
iree_status_t loom_vector_transpose_to_shuffle_rewrite_op(
    loom_rewriter_t* rewriter, loom_op_t* op, bool* out_rewritten);

// Rewrites one vector floating extremum or clamp into exact comparisons,
// same-width integer bit operations, and selects. The replacement preserves
// source payloads and performs no floating-point narrowing. Returns false for
// operations outside the supported extremum family.
iree_status_t loom_vector_float_extrema_rewrite_op(loom_rewriter_t* rewriter,
                                                   loom_op_t* op,
                                                   bool* out_rewritten);

// Applies the exact shared F32 vector extremum or clamp recipe when source
// vector carriers remain legal. Target providers use this callback to opt
// rejected native forms into portable decomposition.
iree_status_t loom_vector_legalize_f32_extrema(
    const loom_target_legalizer_entry_t* entry,
    loom_target_legalization_context_t* context, loom_op_t* op,
    loom_target_legalizer_result_t* out_result);

// Returns the generic vector legalizer provider. Pipelines should compose this
// after target-specific providers so native target rewrites win before scalar
// reference decomposition.
const loom_target_legalizer_provider_t* loom_vector_target_legalizer_provider(
    void);

#ifdef __cplusplus
}
#endif

#endif  // LOOM_TRANSFORMS_VECTOR_TARGET_LEGALIZATION_H_
