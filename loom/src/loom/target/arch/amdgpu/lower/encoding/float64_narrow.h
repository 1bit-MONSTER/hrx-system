// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Exact AMDGPU scalar F64 narrowing through split 32-bit integer words.

#ifndef LOOM_TARGET_ARCH_AMDGPU_LOWER_ENCODING_FLOAT64_NARROW_H_
#define LOOM_TARGET_ARCH_AMDGPU_LOWER_ENCODING_FLOAT64_NARROW_H_

#include "loom/codegen/low/lower/lower.h"
#include "loom/target/arch/amdgpu/lower/plan.h"

#ifdef __cplusplus
extern "C" {
#endif

// Selects exact integer rounding for an F64-to-narrow-float conversion when
// the destination format and required descriptors are available.
bool loom_amdgpu_select_f64_narrow_plan(
    const loom_low_descriptor_set_t* descriptor_set,
    loom_scalar_type_t source_type, loom_scalar_type_t result_type,
    loom_amdgpu_f64_narrow_plan_t* out_plan);

// Emits an exact F64-to-narrow-float conversion from the existing two-word
// source carrier and binds the one-word narrow result carrier.
iree_status_t loom_amdgpu_emit_f64_narrow(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    loom_value_id_t source, loom_value_id_t result,
    const loom_amdgpu_f64_narrow_plan_t* plan);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TARGET_ARCH_AMDGPU_LOWER_ENCODING_FLOAT64_NARROW_H_
