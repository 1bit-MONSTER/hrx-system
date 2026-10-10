// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Wasm SIMD128 lowering for packet-preserving vector structure.

#ifndef LOOM_TARGET_EMIT_WASM_LOWER_VECTOR_STRUCTURAL_H_
#define LOOM_TARGET_EMIT_WASM_LOWER_VECTOR_STRUCTURAL_H_

#include "loom/codegen/low/lower/lower.h"

#ifdef __cplusplus
extern "C" {
#endif

// Queries target-owned structural vector forms implemented with v128 tuples
// and byte shuffles.
iree_status_t loom_wasm_query_vector_structural_contract(
    void* user_data,
    const loom_target_contract_query_environment_t* environment,
    const loom_op_t* source_op,
    loom_target_contract_query_result_t* out_result);

// Selects a Wasm tuple or shuffle plan for one supported structural vector op.
// Other operations leave |out_plan| empty.
iree_status_t loom_wasm_select_vector_structural_plan(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    loom_low_lower_plan_t* out_plan);

// Returns true when |plan| is a Wasm structural vector plan.
bool loom_wasm_vector_structural_plan_isa(loom_low_lower_plan_t plan);

// Retains the source values consumed by |plan|.
void loom_wasm_mark_vector_structural_plan_demands(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    loom_low_lower_plan_t plan);

// Describes the selected tuple or byte-shuffle realization.
void loom_wasm_describe_vector_structural_plan(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    loom_low_lower_plan_t plan, loom_low_lower_plan_report_t* out_report);

// Publishes the native result carriers consumed by dependent plans.
iree_status_t loom_wasm_finalize_vector_structural_plan(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    loom_low_lower_plan_t plan);

// Emits the selected tuple or byte-shuffle realization.
iree_status_t loom_wasm_emit_vector_structural_plan(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    loom_low_lower_plan_t plan);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TARGET_EMIT_WASM_LOWER_VECTOR_STRUCTURAL_H_
