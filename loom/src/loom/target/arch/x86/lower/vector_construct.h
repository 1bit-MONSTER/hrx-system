// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Whole-vector construction from ordered scalar lanes on x86.

#ifndef LOOM_TARGET_ARCH_X86_LOWER_VECTOR_CONSTRUCT_H_
#define LOOM_TARGET_ARCH_X86_LOWER_VECTOR_CONSTRUCT_H_

#include "loom/codegen/low/lower/lower.h"

#ifdef __cplusplus
extern "C" {
#endif

// Queries vector.from_elements forms implemented by compact chunk assembly.
iree_status_t loom_x86_query_vector_construct_contract(
    const loom_target_contract_query_environment_t* environment,
    const loom_op_t* source_op,
    loom_target_contract_query_result_t* out_result);

// Returns true when |plan| is an x86 whole-vector constructor.
bool loom_x86_vector_construct_plan_isa(loom_low_lower_plan_t plan);

// Selects a compact whole-vector constructor. Fully exact native-width numeric
// vectors defer to the generated immutable-load rules.
iree_status_t loom_x86_select_vector_construct_plan(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    loom_low_lower_plan_t* out_plan);

// Marks the source values consumed by a selected constructor. Exact AVX-512
// predicates materialize their packed mask directly and require no operands.
void loom_x86_mark_vector_construct_plan_storage_demands(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    loom_low_lower_plan_t plan);

// Describes the selected constructor for compiler reports.
void loom_x86_describe_vector_construct_plan(
    loom_low_lower_plan_t plan, loom_low_lower_plan_report_t* out_report);

// Emits each 128-bit chunk once and assembles the final carrier once.
iree_status_t loom_x86_emit_vector_construct_plan(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    loom_low_lower_plan_t plan);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TARGET_ARCH_X86_LOWER_VECTOR_CONSTRUCT_H_
