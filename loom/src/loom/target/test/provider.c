// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/target/test/provider.h"

#include "loom/pass/test/registry.h"
#include "loom/target/low_descriptor_registry_core_test.h"
#include "loom/target/test/lower.h"
#include "loom/target/test/target_records.h"

static void loom_test_widen_f32_round_math_policy_query(
    const loom_target_math_policy_t* policy,
    const loom_target_math_query_t* query,
    loom_target_math_policy_decision_t* out_decision) {
  (void)policy;
  if (query->math_op == LOOM_TARGET_MATH_OP_MULF &&
      (query->element_type == LOOM_SCALAR_TYPE_F16 ||
       query->element_type == LOOM_SCALAR_TYPE_BF16)) {
    *out_decision = (loom_target_math_policy_decision_t){
        .action = LOOM_TARGET_MATH_POLICY_ACTION_REWRITE,
        .recipe = LOOM_TARGET_MATH_RECIPE_WIDEN_F32_ROUND,
        .constraint_key = IREE_SVL("test.math.recipe.widen_f32_round"),
    };
    return;
  }
  *out_decision = (loom_target_math_policy_decision_t){
      .action = query->math_op == LOOM_TARGET_MATH_OP_MULF &&
                        query->element_type == LOOM_SCALAR_TYPE_F32
                    ? LOOM_TARGET_MATH_POLICY_ACTION_KEEP
                    : LOOM_TARGET_MATH_POLICY_ACTION_REJECT,
      .constraint_key = IREE_SVL("test.math.widen_f32_round.fixture"),
  };
}

static loom_target_math_policy_decision_t
loom_test_grouped_product_evaluation_rewrite(
    const loom_target_math_policy_t* policy, loom_target_math_recipe_t recipe,
    iree_string_view_t key) {
  return (loom_target_math_policy_decision_t){
      .action = LOOM_TARGET_MATH_POLICY_ACTION_REWRITE,
      .recipe = recipe,
      .evaluation = *(const loom_target_math_evaluation_t*)policy->user_data,
      .constraint_key = key,
  };
}

static void loom_test_grouped_product_evaluation_math_policy_query(
    const loom_target_math_policy_t* policy,
    const loom_target_math_query_t* query,
    loom_target_math_policy_decision_t* out_decision) {
  if (!iree_any_bit_set(query->fastmath_flags,
                        LOOM_TARGET_MATH_FASTMATH_FLAG_AFN)) {
    *out_decision = (loom_target_math_policy_decision_t){
        .action = LOOM_TARGET_MATH_POLICY_ACTION_REJECT,
        .constraint_key = IREE_SVL("test.math.grouped_product.requires_afn"),
    };
    return;
  }
  switch (query->math_op) {
    case LOOM_TARGET_MATH_OP_GELUF_TANH:
      *out_decision = loom_test_grouped_product_evaluation_rewrite(
          policy, LOOM_TARGET_MATH_RECIPE_GELU_TANH_F32,
          IREE_SV("test.math.recipe.gelu_tanh"));
      return;
    case LOOM_TARGET_MATH_OP_GELUF_LOGISTIC:
      *out_decision = loom_test_grouped_product_evaluation_rewrite(
          policy, LOOM_TARGET_MATH_RECIPE_GELU_LOGISTIC_F32,
          IREE_SV("test.math.recipe.gelu_logistic"));
      return;
    case LOOM_TARGET_MATH_OP_SILUF:
      *out_decision = loom_test_grouped_product_evaluation_rewrite(
          policy, LOOM_TARGET_MATH_RECIPE_SILU_LOGISTIC_F32,
          IREE_SV("test.math.recipe.silu_logistic"));
      return;
    case LOOM_TARGET_MATH_OP_LOGISTICF:
      *out_decision = loom_test_grouped_product_evaluation_rewrite(
          policy, LOOM_TARGET_MATH_RECIPE_LOGISTIC_TANH_F32,
          IREE_SV("test.math.recipe.logistic_tanh"));
      return;
    case LOOM_TARGET_MATH_OP_TANHF:
    case LOOM_TARGET_MATH_OP_ADDF:
      *out_decision = (loom_target_math_policy_decision_t){
          .action = LOOM_TARGET_MATH_POLICY_ACTION_KEEP,
          .constraint_key = IREE_SVL("test.math.grouped_product.native"),
      };
      return;
    default:
      *out_decision = (loom_target_math_policy_decision_t){
          .action = LOOM_TARGET_MATH_POLICY_ACTION_REJECT,
          .constraint_key = IREE_SVL("test.math.grouped_product.fixture"),
      };
      return;
  }
}

static void loom_test_math_policy_registry_initialize(
    loom_target_math_policy_registry_t* out_registry) {
  static const loom_target_math_policy_t kWidenF32RoundPolicy = {
      .name = IREE_SVL("test-widen-f32-round"),
      .query = loom_test_widen_f32_round_math_policy_query,
  };
  static const loom_target_math_evaluation_t kBf16x16Evaluation = {
      .kind = LOOM_TARGET_MATH_EVALUATION_GROUPED_PRODUCT,
      .product_element_type = LOOM_SCALAR_TYPE_BF16,
      .accumulator_element_type = LOOM_SCALAR_TYPE_F32,
      .packet_lane_count = 16,
  };
  static const loom_target_math_policy_t kBf16x16EvaluationPolicy = {
      .name = IREE_SVL("test-grouped-product-bf16x16"),
      .query = loom_test_grouped_product_evaluation_math_policy_query,
      .user_data = &kBf16x16Evaluation,
  };
  static const loom_target_math_evaluation_t kF16x4Evaluation = {
      .kind = LOOM_TARGET_MATH_EVALUATION_GROUPED_PRODUCT,
      .product_element_type = LOOM_SCALAR_TYPE_F16,
      .accumulator_element_type = LOOM_SCALAR_TYPE_F32,
      .packet_lane_count = 4,
  };
  static const loom_target_math_policy_t kF16x4EvaluationPolicy = {
      .name = IREE_SVL("test-grouped-product-f16x4"),
      .query = loom_test_grouped_product_evaluation_math_policy_query,
      .user_data = &kF16x4Evaluation,
  };
  static const loom_target_math_policy_registry_entry_t kEntries[] = {
      {
          .contract_set_key = IREE_SVL("test.math.widen_f32_round"),
          .policy = &kWidenF32RoundPolicy,
      },
      {
          .contract_set_key = IREE_SVL("test.math.grouped_product"),
          .policy = &kBf16x16EvaluationPolicy,
      },
      {
          .contract_set_key = IREE_SVL("test.math.grouped_product_f16x4"),
          .policy = &kF16x4EvaluationPolicy,
      },
  };
  loom_target_math_policy_registry_initialize_from_entries(
      out_registry, kEntries, IREE_ARRAYSIZE(kEntries));
}

const loom_target_provider_t loom_test_target_provider = {
    .initialize_low_descriptor_registry =
        loom_target_core_test_low_descriptor_registry_initialize,
    .initialize_low_lower_policy_registry =
        loom_test_low_lower_policy_registry_initialize,
    .initialize_math_policy_registry =
        loom_test_math_policy_registry_initialize,
    .pass_registry = &loom_test_pass_registry_storage,
    .view_boundary_carrier = LOOM_TARGET_VIEW_BOUNDARY_CARRIER_BUFFER_OFFSET,
    .loop_predicate_carrier = LOOM_TARGET_LOOP_PREDICATE_CARRIER_I32,
    .loop_predicate_max_vector_element_count = 32,
    .target_fact_type = &loom_test_target_fact_type,
};
