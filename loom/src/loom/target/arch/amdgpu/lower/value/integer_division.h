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

typedef uint8_t loom_amdgpu_integer_division_plan_kind_t;
enum loom_amdgpu_integer_division_plan_kind_e {
  // Exact i64 quotient or remainder by a compile-time constant.
  LOOM_AMDGPU_INTEGER_DIVISION_PLAN_CONSTANT_I64 = 0,
  // Exact i32 or u32-index quotient and remainder by a runtime value.
  LOOM_AMDGPU_INTEGER_DIVISION_PLAN_DYNAMIC_U32 = 1,
};

// Retains one exact constant-i64 or runtime-u32 division recipe in the
// per-operation plan arena.
typedef struct loom_amdgpu_integer_division_plan_t {
  // Dividend whose complete payload participates in division.
  loom_value_id_t lhs;
  // Runtime divisor, or invalid when |divisor| is an immediate.
  loom_value_id_t rhs;
  // Nonzero divisor bit pattern in the source i64 domain.
  uint64_t divisor;
  // Exact reciprocal recipe selected by source operation signedness.
  union {
    // Unsigned recipe, unused for the identity divisor one.
    loom_low_lower_unsigned_divisor_magic_info_t unsigned_value;
    // Signed recipe, unused for divisors one and negative one.
    loom_low_lower_signed_divisor_magic_info_t signed_value;
  } magic;
  // Selected SGPR or VGPR carrier for the computation.
  uint16_t register_class_id;
  // Constant-i64 or dynamic-u32 recipe discriminator.
  loom_amdgpu_integer_division_plan_kind_t kind;
  // One or two words needed by an unsigned quotient reconstruction.
  uint8_t quotient_unit_count;
} loom_amdgpu_integer_division_plan_t;

// Selects exact signed or unsigned constant-i64 and runtime-u32 division.
iree_status_t loom_amdgpu_select_integer_division_plan(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    loom_amdgpu_integer_division_plan_t* out_plan, bool* out_selected);

// Publishes the result carriers selected by the retained division plan.
iree_status_t loom_amdgpu_finalize_integer_division_plan(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    const loom_amdgpu_integer_division_plan_t* plan);

// Marks the dividend and any runtime divisor that emission must consume.
void loom_amdgpu_mark_integer_division_plan_storage_demands(
    loom_low_lower_context_t* context,
    const loom_amdgpu_integer_division_plan_t* plan);

// Emits the retained reciprocal recipe using native word arithmetic.
iree_status_t loom_amdgpu_lower_integer_division(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    const loom_amdgpu_integer_division_plan_t* plan);

// Verifies constant-divisor and descriptor requirements for i64 division.
iree_status_t loom_amdgpu_low_legality_verify_i64_division(
    const loom_target_low_legality_provider_t* provider,
    loom_target_low_legality_context_t* context, const loom_op_t* op,
    bool* out_handled);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TARGET_ARCH_AMDGPU_LOWER_VALUE_INTEGER_DIVISION_H_
