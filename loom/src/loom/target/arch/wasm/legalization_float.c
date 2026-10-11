// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/target/arch/wasm/legalization_float.h"

#include "loom/ir/module.h"
#include "loom/ops/vector/ops.h"
#include "loom/target/arch/wasm/descriptors/descriptors.h"
#include "loom/transforms/vector/target_legalization.h"

static bool loom_wasm_float_legalizer_supports_descriptor_set(
    const loom_low_descriptor_set_t* descriptor_set) {
  return descriptor_set != NULL &&
         descriptor_set->target_stable_id ==
             loom_wasm_core_simd128_descriptor_set()->target_stable_id;
}

static loom_type_t loom_wasm_vector_type_with_element(
    loom_type_t source_type, loom_scalar_type_t element_type) {
  source_type.header = loom_type_make_header(
      loom_type_kind(source_type), element_type, loom_type_rank(source_type),
      loom_type_flags(source_type));
  return source_type;
}

static iree_status_t loom_wasm_float_comparison_widen_operand(
    loom_builder_t* builder, loom_value_id_t operand, loom_type_t source_type,
    loom_type_t comparison_type, loom_location_id_t location,
    loom_value_id_t* out_operand) {
  if (loom_type_equal(source_type, comparison_type)) {
    *out_operand = operand;
    return iree_ok_status();
  }
  loom_op_t* extension_op = NULL;
  IREE_RETURN_IF_ERROR(loom_vector_extf_build(
      builder, /*instance_flags=*/0, operand, source_type, comparison_type,
      location, &extension_op));
  *out_operand = loom_vector_extf_result(extension_op);
  return iree_ok_status();
}

iree_status_t loom_wasm_legalize_vector_cmpf(
    const loom_target_legalizer_entry_t* entry,
    loom_target_legalization_context_t* context, loom_op_t* op,
    loom_target_legalizer_result_t* out_result) {
  (void)entry;
  *out_result = (loom_target_legalizer_result_t){
      .action = LOOM_TARGET_LEGALIZER_ACTION_NO_COMMENT,
  };
  if (!loom_wasm_float_legalizer_supports_descriptor_set(
          context->descriptor_set) ||
      !loom_target_legalization_op_has_source_vector_carriers(context, op)) {
    return iree_ok_status();
  }

  loom_rewriter_t* rewriter = context->rewriter;
  loom_builder_t* builder = &rewriter->builder;
  loom_builder_set_before(builder, op);
  const loom_value_id_t checkpoint = loom_rewriter_value_checkpoint(rewriter);
  const loom_value_id_t lhs = loom_vector_cmpf_lhs(op);
  const loom_value_id_t rhs = loom_vector_cmpf_rhs(op);
  const loom_type_t source_type = loom_module_value_type(context->module, lhs);
  const loom_type_t comparison_type =
      loom_wasm_vector_type_with_element(source_type, LOOM_SCALAR_TYPE_F32);
  loom_value_id_t comparison_lhs = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_wasm_float_comparison_widen_operand(
      builder, lhs, source_type, comparison_type, op->location,
      &comparison_lhs));
  loom_value_id_t comparison_rhs = comparison_lhs;
  if (rhs != lhs) {
    IREE_RETURN_IF_ERROR(loom_wasm_float_comparison_widen_operand(
        builder, rhs, source_type, comparison_type, op->location,
        &comparison_rhs));
  }

  const loom_type_t result_type =
      loom_module_value_type(context->module, loom_vector_cmpf_result(op));
  loom_op_t* comparison_op = NULL;
  IREE_RETURN_IF_ERROR(loom_vector_cmpf_build(
      builder, loom_vector_cmpf_fastmath(op), loom_vector_cmpf_predicate(op),
      comparison_lhs, comparison_rhs, comparison_type, result_type,
      op->location, &comparison_op));
  const loom_value_id_t replacement = loom_vector_cmpf_result(comparison_op);
  IREE_RETURN_IF_ERROR(loom_rewriter_preserve_result_names_on_new_values(
      rewriter, op, &replacement, 1, checkpoint));
  IREE_RETURN_IF_ERROR(
      loom_rewriter_replace_all_uses_and_erase(rewriter, op, &replacement, 1));
  out_result->action = LOOM_TARGET_LEGALIZER_ACTION_REWRITTEN;
  return iree_ok_status();
}

iree_status_t loom_wasm_legalize_vector_float_extrema(
    const loom_target_legalizer_entry_t* entry,
    loom_target_legalization_context_t* context, loom_op_t* op,
    loom_target_legalizer_result_t* out_result) {
  (void)entry;
  *out_result = (loom_target_legalizer_result_t){
      .action = LOOM_TARGET_LEGALIZER_ACTION_NO_COMMENT,
  };
  if (!loom_wasm_float_legalizer_supports_descriptor_set(
          context->descriptor_set) ||
      !loom_target_legalization_op_has_source_vector_carriers(context, op)) {
    return iree_ok_status();
  }
  bool rewritten = false;
  IREE_RETURN_IF_ERROR(
      loom_vector_float_extrema_rewrite_op(context->rewriter, op, &rewritten));
  if (rewritten) {
    out_result->action = LOOM_TARGET_LEGALIZER_ACTION_REWRITTEN;
  }
  return iree_ok_status();
}
