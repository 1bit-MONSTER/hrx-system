// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/target/arch/spirv/legalization.h"

#include "loom/ir/module.h"
#include "loom/ops/scalar/ops.h"
#include "loom/ops/vector/ops.h"
#include "loom/rewrite/rewriter.h"
#include "loom/target/arch/spirv/descriptors/descriptors.h"
#include "loom/transforms/vector/to_scalar.h"

static bool loom_spirv_legalizer_descriptor_set_is_spirv(
    const loom_low_descriptor_set_t* descriptor_set) {
  return descriptor_set != NULL &&
         descriptor_set->target_stable_id ==
             loom_spirv_logical_core_descriptor_set()->target_stable_id;
}

static iree_status_t loom_spirv_legalize_vector_load(
    const loom_target_legalizer_entry_t* entry,
    loom_target_legalization_context_t* context, loom_op_t* op,
    loom_target_legalizer_result_t* out_result) {
  (void)entry;
  *out_result = (loom_target_legalizer_result_t){
      .action = LOOM_TARGET_LEGALIZER_ACTION_NO_COMMENT,
  };
  if (!loom_spirv_legalizer_descriptor_set_is_spirv(context->descriptor_set)) {
    return iree_ok_status();
  }

  bool rewritten = false;
  // Capture every lane where the vector read occurs. Delaying reads until
  // arithmetic consumers would change the snapshot across aliasing writes.
  IREE_RETURN_IF_ERROR(loom_vector_descriptor_to_scalar_rewrite_op(
      context->pass, context->rewriter, op, &rewritten));
  if (rewritten) {
    out_result->action = LOOM_TARGET_LEGALIZER_ACTION_REWRITTEN;
  }
  return iree_ok_status();
}

static bool loom_spirv_match_float8_to_bfloat_extension(
    const loom_target_legalizer_entry_t* entry,
    const loom_target_legalization_context_t* context, const loom_op_t* op) {
  (void)entry;
  const loom_type_t result_type =
      loom_module_value_type(context->module, loom_scalar_extf_result(op));
  return loom_type_is_scalar(result_type) &&
         loom_type_element_type(result_type) == LOOM_SCALAR_TYPE_BF16;
}

static iree_status_t loom_spirv_legalize_float8_to_bfloat_extension(
    const loom_target_legalizer_entry_t* entry,
    loom_target_legalization_context_t* context, loom_op_t* op,
    loom_target_legalizer_result_t* out_result) {
  (void)entry;
  *out_result = (loom_target_legalizer_result_t){
      .action = LOOM_TARGET_LEGALIZER_ACTION_NO_COMMENT,
  };
  if (!loom_spirv_legalizer_descriptor_set_is_spirv(context->descriptor_set)) {
    return iree_ok_status();
  }

  loom_rewriter_t* rewriter = context->rewriter;
  loom_builder_t* builder = &rewriter->builder;
  loom_builder_set_before(builder, op);
  const loom_value_id_t checkpoint = loom_rewriter_value_checkpoint(rewriter);
  const loom_value_id_t input = loom_scalar_extf_input(op);
  const loom_type_t input_type = loom_module_value_type(context->module, input);
  const loom_type_t f32_type = loom_type_scalar(LOOM_SCALAR_TYPE_F32);
  const loom_type_t result_type =
      loom_module_value_type(context->module, loom_scalar_extf_result(op));
  // F32 represents every FP8 value exactly. Staging through it preserves the
  // extension semantics while reusing the existing FP8 decode and exact BF16
  // narrowing paths, including their special-value handling.
  loom_op_t* widened = NULL;
  IREE_RETURN_IF_ERROR(loom_scalar_extf_build(
      builder, input, input_type, f32_type, op->location, &widened));
  loom_op_t* narrowed = NULL;
  IREE_RETURN_IF_ERROR(loom_scalar_fptrunc_build(
      builder, loom_scalar_extf_result(widened), f32_type, result_type,
      op->location, &narrowed));
  const loom_value_id_t replacement = loom_scalar_fptrunc_result(narrowed);
  IREE_RETURN_IF_ERROR(loom_rewriter_preserve_result_names_on_new_values(
      rewriter, op, &replacement, 1, checkpoint));
  IREE_RETURN_IF_ERROR(
      loom_rewriter_replace_all_uses_and_erase(rewriter, op, &replacement, 1));
  out_result->action = LOOM_TARGET_LEGALIZER_ACTION_REWRITTEN;
  return iree_ok_status();
}

static const loom_target_legalizer_rule_t kSpirvLegalizerRules[] = {
    {
        .root_kind = LOOM_OP_SCALAR_EXTF,
        .first_operand_element_types =
            LOOM_SCALAR_TYPE_SET_F8E4M3 | LOOM_SCALAR_TYPE_SET_F8E5M2,
        .match = loom_spirv_match_float8_to_bfloat_extension,
        .legalize = loom_spirv_legalize_float8_to_bfloat_extension,
    },
    {
        .root_kind = LOOM_OP_VECTOR_LOAD,
        .legalize = loom_spirv_legalize_vector_load,
    },
};

const loom_target_legalizer_provider_t
    loom_spirv_target_legalizer_provider_storage = {
        .name = IREE_SVL("spirv"),
        .strategy = LOOM_TARGET_LEGALIZER_STRATEGY_TARGET,
        .rules = kSpirvLegalizerRules,
        .rule_count = IREE_ARRAYSIZE(kSpirvLegalizerRules),
};

const loom_target_legalizer_provider_t* loom_spirv_target_legalizer_provider(
    void) {
  return &loom_spirv_target_legalizer_provider_storage;
}
