// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// AMDGPU lowering for register table source operations.

#ifndef LOOM_TARGET_ARCH_AMDGPU_LOWER_TABLE_H_
#define LOOM_TARGET_ARCH_AMDGPU_LOWER_TABLE_H_

#include "loom/codegen/low/lower/lower.h"
#include "loom/target/arch/amdgpu/lower/plan.h"
#include "loom/target/low_legality.h"

#ifdef __cplusplus
extern "C" {
#endif

// Selects an AMDGPU register-table lookup plan.
iree_status_t loom_amdgpu_select_vector_table_lookup_plan(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    loom_amdgpu_table_lookup_plan_t* out_plan, bool* out_selected);

// Lowers a source vector.table.lookup op with its selected native recipe.
// Packed byte recipes emit independent permutes for each result register,
// including the semantic low bytes of a partial final register.
iree_status_t loom_amdgpu_lower_vector_table_lookup(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    const loom_amdgpu_table_lookup_plan_t* plan);

// Returns whether the current shapes, index facts and target descriptors admit
// a native recipe. Shared legalization uses this same selection predicate to
// preserve native lookup forms while expanding unsupported forms.
bool loom_amdgpu_vector_table_lookup_is_supported(
    const loom_module_t* module, const loom_value_fact_table_t* fact_table,
    const loom_low_descriptor_set_t* descriptor_set, const loom_op_t* op);

// Verifies source vector table op legality for AMDGPU target-low selection.
iree_status_t loom_amdgpu_low_legality_verify_vector_table(
    const loom_target_low_legality_provider_t* provider,
    loom_target_low_legality_context_t* context, const loom_op_t* op,
    bool* out_handled);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TARGET_ARCH_AMDGPU_LOWER_TABLE_H_
