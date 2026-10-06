// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Exact AMDGPU materialization of packed E8M0 scale values.

#ifndef LOOM_TARGET_ARCH_AMDGPU_LOWER_ENCODING_E8M0_SCALE_H_
#define LOOM_TARGET_ARCH_AMDGPU_LOWER_ENCODING_E8M0_SCALE_H_

#include "loom/codegen/low/lower/lower.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
  // Number of independently addressable E8M0 bytes in one i32 register.
  LOOM_AMDGPU_E8M0_SCALE_VALUES_PER_REGISTER = 4,
};

// Function-local values shared by every E8M0 scale materialized for one
// conversion.
typedef struct loom_amdgpu_e8m0_f32_scale_materializer_t {
  // One-unit VGPR type used for encoded and decoded scale payloads.
  loom_type_t vector_type;
  // EXEC-width lane-mask type produced by scale-byte comparisons.
  loom_type_t mask_type;
  // VGPR carrying the F32 bit pattern for the minimum E8M0 scale.
  loom_value_id_t low_minimum_scale;
  // VGPR carrying the canonical F32 quiet-NaN bit pattern.
  loom_value_id_t low_quiet_nan_scale;
} loom_amdgpu_e8m0_f32_scale_materializer_t;

// Returns true when the descriptor set can materialize exact E8M0 scale
// semantics, including the minimum and NaN encodings.
bool loom_amdgpu_e8m0_f32_scale_materialization_available(
    const loom_low_descriptor_set_t* descriptor_set);

// Initializes values shared by a sequence of packed E8M0 scale decodes.
iree_status_t loom_amdgpu_initialize_e8m0_f32_scale_materializer(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    loom_amdgpu_e8m0_f32_scale_materializer_t* out_materializer);

// Extracts |scale_index| from packed i32 scale registers and returns the exact
// F32 scale bit pattern in one VGPR. Byte zero maps to 2^-127 and byte 255 maps
// to a canonical quiet NaN.
iree_status_t loom_amdgpu_materialize_e8m0_f32_scale(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    const loom_amdgpu_e8m0_f32_scale_materializer_t* materializer,
    loom_value_id_t low_scale_source, uint32_t scale_register_count,
    uint32_t scale_index, loom_value_id_t* out_low_f32_scale);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TARGET_ARCH_AMDGPU_LOWER_ENCODING_E8M0_SCALE_H_
