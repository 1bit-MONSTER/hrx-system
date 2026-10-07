// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_TARGET_ARCH_AMDGPU_LOWER_VALUE_INTEGER_DIVISION_H_
#define LOOM_TARGET_ARCH_AMDGPU_LOWER_VALUE_INTEGER_DIVISION_H_

#include "loom/codegen/low/lower/lower.h"
#include "loom/codegen/low/lower/rule_value.h"
#include "loom/target/low_legality.h"

#ifdef __cplusplus
extern "C" {
#endif

// Retains the exact divisor recipe in the existing per-operation plan arena.
// Only the numerator demands source storage; the divisor is an immediate.
typedef struct loom_amdgpu_unsigned_i64_division_plan_t {
  // Numerator whose complete unsigned payload participates in division.
  loom_value_id_t source;
  // Source quotient or remainder result bound by emission.
  loom_value_id_t result;
  // Nonzero unsigned divisor, including high-bit signed literal spellings.
  uint64_t divisor;
  // Exact reciprocal recipe, unused for the identity divisor one.
  loom_low_lower_unsigned_divisor_magic_info_t magic;
  // Selected SGPR or VGPR carrier for both words of the computation.
  uint16_t register_class_id;
} loom_amdgpu_unsigned_i64_division_plan_t;

// Selects exact unsigned i64 division or remainder by a constant divisor.
iree_status_t loom_amdgpu_select_unsigned_i64_division_plan(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    loom_amdgpu_unsigned_i64_division_plan_t* out_plan, bool* out_selected);

// Emits the retained reciprocal recipe using native word arithmetic.
iree_status_t loom_amdgpu_lower_unsigned_i64_division(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    const loom_amdgpu_unsigned_i64_division_plan_t* plan);

// Verifies constant-divisor and descriptor requirements for unsigned i64.
iree_status_t loom_amdgpu_low_legality_verify_unsigned_i64_division(
    const loom_target_low_legality_provider_t* provider,
    loom_target_low_legality_context_t* context, const loom_op_t* op,
    bool* out_handled);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TARGET_ARCH_AMDGPU_LOWER_VALUE_INTEGER_DIVISION_H_
