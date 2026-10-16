// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/target/arch/wasm/math_policy.h"

#include <stddef.h>

#define LOOM_WASM_MATH_CONSTRAINTS(_)                   \
  _(ELEMENT_F32, element_f32, "math.element.f32")       \
  _(OP_NATIVE_F32, op_native_f32, "math.op.native_f32") \
  _(OP_SUPPORTED, op_supported, "math.op.supported")    \
  _(RECIPE_RSQRT_SQRT_DIV, recipe_rsqrt_sqrt_div, "math.recipe.rsqrt_sqrt_div")

enum loom_wasm_math_constraint_e {
  LOOM_WASM_MATH_CONSTRAINT_NONE = 0,
#define LOOM_WASM_MATH_CONSTRAINT_ENUM(name, field, value) \
  LOOM_WASM_MATH_CONSTRAINT_##name,
  LOOM_WASM_MATH_CONSTRAINTS(LOOM_WASM_MATH_CONSTRAINT_ENUM)
#undef LOOM_WASM_MATH_CONSTRAINT_ENUM
      LOOM_WASM_MATH_CONSTRAINT_COUNT_,
};

typedef struct loom_wasm_math_constraint_strings_t {
#define LOOM_WASM_MATH_CONSTRAINT_FIELD(name, field, value) \
  char field[sizeof(value)];
  LOOM_WASM_MATH_CONSTRAINTS(LOOM_WASM_MATH_CONSTRAINT_FIELD)
#undef LOOM_WASM_MATH_CONSTRAINT_FIELD
} loom_wasm_math_constraint_strings_t;

static const loom_wasm_math_constraint_strings_t kWasmMathConstraintStrings = {
#define LOOM_WASM_MATH_CONSTRAINT_VALUE(name, field, value) .field = value,
    LOOM_WASM_MATH_CONSTRAINTS(LOOM_WASM_MATH_CONSTRAINT_VALUE)
#undef LOOM_WASM_MATH_CONSTRAINT_VALUE
};

static const loom_string_ref_t
    kWasmMathConstraints[LOOM_WASM_MATH_CONSTRAINT_COUNT_ - 1] = {
#define LOOM_WASM_MATH_CONSTRAINT_REF(name, field, value)                      \
  [LOOM_WASM_MATH_CONSTRAINT_##name - 1] =                                     \
      LOOM_STRING_REF(0, offsetof(loom_wasm_math_constraint_strings_t, field), \
                      sizeof(value) - 1),
        LOOM_WASM_MATH_CONSTRAINTS(LOOM_WASM_MATH_CONSTRAINT_REF)
#undef LOOM_WASM_MATH_CONSTRAINT_REF
};

#undef LOOM_WASM_MATH_CONSTRAINTS

#define LOOM_WASM_MATH_DIRECT_RULE(element_types_, recipe_, constraint_) \
  LOOM_TARGET_MATH_POLICY_DIRECT_RULE(                                   \
      element_types_, LOOM_TARGET_MATH_LANE_DOMAIN_SET_ALL,              \
      LOOM_TARGET_MATH_FASTMATH_FLAG_NONE, recipe_,                      \
      LOOM_TARGET_MATH_FASTMATH_FLAG_NONE,                               \
      LOOM_TARGET_MATH_FASTMATH_FLAG_NONE, 0, constraint_,               \
      LOOM_WASM_MATH_CONSTRAINT_NONE)

static const loom_target_math_policy_direct_rule_t
    kWasmMathDirectRules[LOOM_TARGET_MATH_OP_COUNT] = {
        [LOOM_TARGET_MATH_OP_ADDF] = LOOM_WASM_MATH_DIRECT_RULE(
            LOOM_SCALAR_TYPE_SET_F32, LOOM_TARGET_MATH_RECIPE_UNKNOWN,
            LOOM_WASM_MATH_CONSTRAINT_OP_NATIVE_F32),
        [LOOM_TARGET_MATH_OP_SUBF] = LOOM_WASM_MATH_DIRECT_RULE(
            LOOM_SCALAR_TYPE_SET_F32, LOOM_TARGET_MATH_RECIPE_UNKNOWN,
            LOOM_WASM_MATH_CONSTRAINT_OP_NATIVE_F32),
        [LOOM_TARGET_MATH_OP_MULF] = LOOM_WASM_MATH_DIRECT_RULE(
            LOOM_SCALAR_TYPE_SET_F32, LOOM_TARGET_MATH_RECIPE_UNKNOWN,
            LOOM_WASM_MATH_CONSTRAINT_OP_NATIVE_F32),
        [LOOM_TARGET_MATH_OP_CEILF] = LOOM_WASM_MATH_DIRECT_RULE(
            LOOM_SCALAR_TYPE_SET_F32, LOOM_TARGET_MATH_RECIPE_UNKNOWN,
            LOOM_WASM_MATH_CONSTRAINT_OP_NATIVE_F32),
        [LOOM_TARGET_MATH_OP_FLOORF] = LOOM_WASM_MATH_DIRECT_RULE(
            LOOM_SCALAR_TYPE_SET_F32, LOOM_TARGET_MATH_RECIPE_UNKNOWN,
            LOOM_WASM_MATH_CONSTRAINT_OP_NATIVE_F32),
        [LOOM_TARGET_MATH_OP_ROUNDEVENF] = LOOM_WASM_MATH_DIRECT_RULE(
            LOOM_SCALAR_TYPE_SET_F32, LOOM_TARGET_MATH_RECIPE_UNKNOWN,
            LOOM_WASM_MATH_CONSTRAINT_OP_NATIVE_F32),
        [LOOM_TARGET_MATH_OP_TRUNCF] = LOOM_WASM_MATH_DIRECT_RULE(
            LOOM_SCALAR_TYPE_SET_F32, LOOM_TARGET_MATH_RECIPE_UNKNOWN,
            LOOM_WASM_MATH_CONSTRAINT_OP_NATIVE_F32),
        [LOOM_TARGET_MATH_OP_SQRTF] = LOOM_WASM_MATH_DIRECT_RULE(
            LOOM_SCALAR_TYPE_SET_F32, LOOM_TARGET_MATH_RECIPE_UNKNOWN,
            LOOM_WASM_MATH_CONSTRAINT_OP_NATIVE_F32),
        [LOOM_TARGET_MATH_OP_RSQRTF] = LOOM_WASM_MATH_DIRECT_RULE(
            LOOM_SCALAR_TYPE_SET_F32, LOOM_TARGET_MATH_RECIPE_RSQRT_SQRT_DIV,
            LOOM_WASM_MATH_CONSTRAINT_RECIPE_RSQRT_SQRT_DIV),
};

#undef LOOM_WASM_MATH_DIRECT_RULE

#define LOOM_WASM_MATH_F32_SPAN \
  {.fallback_constraint = LOOM_WASM_MATH_CONSTRAINT_ELEMENT_F32}

static const loom_target_math_policy_rule_span_t
    kWasmMathOperationSpans[LOOM_TARGET_MATH_OP_COUNT] = {
        [LOOM_TARGET_MATH_OP_ADDF] = LOOM_WASM_MATH_F32_SPAN,
        [LOOM_TARGET_MATH_OP_SUBF] = LOOM_WASM_MATH_F32_SPAN,
        [LOOM_TARGET_MATH_OP_MULF] = LOOM_WASM_MATH_F32_SPAN,
        [LOOM_TARGET_MATH_OP_CEILF] = LOOM_WASM_MATH_F32_SPAN,
        [LOOM_TARGET_MATH_OP_FLOORF] = LOOM_WASM_MATH_F32_SPAN,
        [LOOM_TARGET_MATH_OP_ROUNDEVENF] = LOOM_WASM_MATH_F32_SPAN,
        [LOOM_TARGET_MATH_OP_TRUNCF] = LOOM_WASM_MATH_F32_SPAN,
        [LOOM_TARGET_MATH_OP_SQRTF] = LOOM_WASM_MATH_F32_SPAN,
        [LOOM_TARGET_MATH_OP_RSQRTF] = LOOM_WASM_MATH_F32_SPAN,
};

#undef LOOM_WASM_MATH_F32_SPAN

static const loom_target_math_policy_table_t kWasmMathPolicyTable = {
    .operation_spans = kWasmMathOperationSpans,
    .default_constraint = LOOM_WASM_MATH_CONSTRAINT_OP_SUPPORTED,
    .constraints = kWasmMathConstraints,
    .constraint_strings =
        {
            .data = {(const char*)&kWasmMathConstraintStrings},
            .data_length = {sizeof(kWasmMathConstraintStrings)},
        },
};

static const loom_target_math_policy_t kWasmMathPolicy = {
    .name = IREE_SVL("wasm-math"),
    .direct_rules = kWasmMathDirectRules,
    .table = &kWasmMathPolicyTable,
};

static const loom_target_math_policy_registry_entry_t kWasmMathPolicyEntries[] =
    {
        {/*.contract_set_key=*/IREE_SVL("wasm.core.simd128"),
         /*.policy=*/&kWasmMathPolicy},
};

void loom_wasm_math_policy_registry_initialize(
    loom_target_math_policy_registry_t* out_registry) {
  loom_target_math_policy_registry_initialize_from_entries(
      out_registry, kWasmMathPolicyEntries,
      IREE_ARRAYSIZE(kWasmMathPolicyEntries));
}
