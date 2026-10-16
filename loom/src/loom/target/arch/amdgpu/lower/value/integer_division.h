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

// Retains an exact wide constant-divisor recipe in the per-operation plan
// arena. Only the numerator demands source storage; the divisor is an
// immediate.
typedef struct loom_amdgpu_wide_division_plan_t {
  // Numerator whose complete payload participates in division.
  loom_value_id_t source;
  // Source-level quotient result bound by emission, or invalid when unused.
  loom_value_id_t quotient;
  // Source-level remainder result bound by emission, or invalid when unused.
  loom_value_id_t remainder;
  // Nonzero divisor bit pattern in the source operation's 64-bit domain.
  uint64_t divisor;
  // Exact reciprocal recipe selected by source operation signedness.
  union {
    // Unsigned recipe, unused for the identity divisor one.
    loom_low_lower_unsigned_divisor_magic_info_t unsigned_value;
    // Signed recipe, unused for divisors one and negative one.
    loom_low_lower_signed_divisor_magic_info_t signed_value;
  } magic;
  // Selected SGPR or VGPR carrier for both words of the computation.
  uint16_t register_class_id;
  // One or two words needed by an unsigned quotient reconstruction.
  uint8_t quotient_unit_count;
  // One or two words published for the quotient result.
  uint8_t quotient_result_unit_count;
  // One or two words published for the remainder result.
  uint8_t remainder_result_unit_count;
} loom_amdgpu_wide_division_plan_t;

// Selects exact signed-i64, unsigned-i64, or wide-index division by a
// constant.
iree_status_t loom_amdgpu_select_wide_division_plan(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    loom_amdgpu_wide_division_plan_t* out_plan, bool* out_selected);

// Publishes each result carrier selected for the retained wide computation.
iree_status_t loom_amdgpu_finalize_wide_division_plan(
    loom_low_lower_context_t* context,
    const loom_amdgpu_wide_division_plan_t* plan);

// Emits the retained reciprocal recipe using native word arithmetic.
iree_status_t loom_amdgpu_lower_wide_division(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    const loom_amdgpu_wide_division_plan_t* plan);

// Verifies constant-divisor and descriptor requirements for wide division.
iree_status_t loom_amdgpu_low_legality_verify_wide_division(
    const loom_target_low_legality_provider_t* provider,
    loom_target_low_legality_context_t* context, const loom_op_t* op,
    bool* out_handled);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TARGET_ARCH_AMDGPU_LOWER_VALUE_INTEGER_DIVISION_H_
