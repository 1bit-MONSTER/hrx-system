// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Exact AMDGPU floating-point to 64-bit integer conversion.

#ifndef LOOM_TARGET_ARCH_AMDGPU_LOWER_ENCODING_FLOAT_TO_INTEGER64_H_
#define LOOM_TARGET_ARCH_AMDGPU_LOWER_ENCODING_FLOAT_TO_INTEGER64_H_

#include "loom/codegen/low/lower/lower.h"
#include "loom/target/arch/amdgpu/lower/plan.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct loom_amdgpu_float_to_integer64_emission_t {
  // One-word VGPR type used by integer results and F32 intermediates.
  loom_type_t word_type;
  // Two-word VGPR type used by integer results and F64 intermediates.
  loom_type_t pair_type;
  // Working F32 or F64 VGPR type selected by the semantic plan.
  loom_type_t floating_type;
  // SGPR exponent value -32 shared by every converted lane.
  loom_value_id_t split_exponent;
  // SGPR F32 or F64 value -2^32 shared by every converted lane.
  loom_value_id_t negative_word_scale;
} loom_amdgpu_float_to_integer64_emission_t;

// Selects an exact split-word conversion for BF16, F32, or F64 sources when
// the destination interpretation and all required descriptors are available.
// BF16 sources use the F32 plan after exact bit-alignment by the caller.
bool loom_amdgpu_select_float_to_integer64_plan(
    const loom_low_descriptor_set_t* descriptor_set,
    loom_scalar_type_t source_type,
    loom_amdgpu_float_to_integer64_signedness_t signedness,
    loom_amdgpu_float_to_integer64_plan_t* out_plan);

// Emits the constants shared by all lanes converted under |plan|.
iree_status_t loom_amdgpu_initialize_float_to_integer64_emission(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    const loom_amdgpu_float_to_integer64_plan_t* plan,
    loom_amdgpu_float_to_integer64_emission_t* out_emission);

// Converts one normalized F32 or F64 VGPR lane to a two-word VGPR integer.
// The caller exactly aligns BF16 bits into an F32 carrier before calling.
iree_status_t loom_amdgpu_emit_float_to_integer64(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    const loom_amdgpu_float_to_integer64_plan_t* plan,
    const loom_amdgpu_float_to_integer64_emission_t* emission,
    loom_value_id_t low_source, loom_value_id_t* out_low_result);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TARGET_ARCH_AMDGPU_LOWER_ENCODING_FLOAT_TO_INTEGER64_H_
