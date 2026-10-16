// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/transforms/symbol/template_rewrite.h"

#include "loom/ir/module.h"
#include "loom/ops/template/ops.h"
#include "loom/rewrite/rewriter.h"

iree_status_t loom_template_rewrite_apply_as_exact_call(
    loom_rewriter_t* rewriter, loom_op_t* apply_op, loom_symbol_ref_t callee,
    const loom_value_id_t* operands, loom_op_t** out_call_op) {
  IREE_ASSERT(loom_template_apply_isa(apply_op));
  const loom_value_slice_t results = loom_template_apply_results(apply_op);
  loom_type_t* result_types = NULL;
  if (results.count > 0) {
    IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
        rewriter->arena, results.count, sizeof(*result_types),
        (void**)&result_types));
    for (uint16_t i = 0; i < results.count; ++i) {
      result_types[i] =
          loom_module_value_type(rewriter->module, results.values[i]);
    }
  }

  loom_template_call_build_flags_t build_flags = 0;
  const loom_callable_effect_contract_t effect_contract =
      loom_template_apply_effect_contract(apply_op);
  if (effect_contract != 0) {
    build_flags |= LOOM_TEMPLATE_CALL_BUILD_FLAG_HAS_EFFECT_CONTRACT;
  }
  const uint8_t temperature = loom_template_apply_temperature(apply_op);
  if (temperature != 0) {
    build_flags |= LOOM_TEMPLATE_CALL_BUILD_FLAG_HAS_TEMPERATURE;
  }

  loom_builder_set_before(&rewriter->builder, apply_op);
  const loom_value_id_t value_checkpoint =
      loom_rewriter_value_checkpoint(rewriter);
  loom_op_t* call_op = NULL;
  IREE_RETURN_IF_ERROR(loom_template_call_build(
      &rewriter->builder, build_flags, effect_contract, temperature, callee,
      operands, apply_op->operand_count, result_types, results.count,
      loom_op_tied_results(apply_op), apply_op->tied_result_count,
      apply_op->location, &call_op));
  const loom_value_slice_t call_results = loom_template_call_results(call_op);
  IREE_RETURN_IF_ERROR(loom_rewriter_preserve_result_names_on_new_values(
      rewriter, apply_op, call_results.values, call_results.count,
      value_checkpoint));
  IREE_RETURN_IF_ERROR(loom_rewriter_replace_all_uses_and_erase(
      rewriter, apply_op, call_results.values, call_results.count));
  if (out_call_op) {
    *out_call_op = call_op;
  }
  return iree_ok_status();
}
