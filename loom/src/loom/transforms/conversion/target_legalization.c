// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/transforms/conversion/target_legalization.h"

#include "loom/ir/module.h"
#include "loom/ops/scalar/ops.h"
#include "loom/ops/vector/ops.h"
#include "loom/rewrite/rewriter.h"

typedef iree_status_t (*loom_conversion_builder_fn_t)(
    loom_builder_t* builder, loom_value_id_t input, loom_type_t input_type,
    loom_type_t result_type, loom_location_id_t location, loom_op_t** out_op);

// Builds a shape-preserving conversion chain with an F32 intermediate. The
// caller establishes that staging preserves the original rounding semantics.
static iree_status_t loom_conversion_stage_f32(
    loom_target_legalization_context_t* context, loom_op_t* op,
    loom_conversion_builder_fn_t first_builder,
    loom_conversion_builder_fn_t second_builder,
    loom_target_legalizer_result_t* out_result) {
  const loom_value_id_t input = loom_op_operands(op)[0];
  const loom_type_t input_type = loom_module_value_type(context->module, input);
  const loom_type_t result_type =
      loom_module_value_type(context->module, loom_op_results(op)[0]);
  loom_type_t intermediate_type = input_type;
  intermediate_type.header = loom_type_make_header(
      loom_type_kind(input_type), LOOM_SCALAR_TYPE_F32,
      loom_type_rank(input_type), loom_type_flags(input_type));
  IREE_RETURN_IF_ERROR(loom_module_intern_type(
      context->module, intermediate_type, &intermediate_type));

  loom_rewriter_t* rewriter = context->rewriter;
  loom_builder_set_before(&rewriter->builder, op);
  const loom_value_id_t checkpoint = loom_rewriter_value_checkpoint(rewriter);
  loom_op_t* intermediate = NULL;
  IREE_RETURN_IF_ERROR(first_builder(&rewriter->builder, input, input_type,
                                     intermediate_type, op->location,
                                     &intermediate));
  loom_op_t* converted = NULL;
  IREE_RETURN_IF_ERROR(
      second_builder(&rewriter->builder, loom_op_results(intermediate)[0],
                     intermediate_type, result_type, op->location, &converted));
  const loom_value_id_t replacement = loom_op_results(converted)[0];
  IREE_RETURN_IF_ERROR(loom_rewriter_preserve_result_names_on_new_values(
      rewriter, op, &replacement, 1, checkpoint));
  IREE_RETURN_IF_ERROR(
      loom_rewriter_replace_all_uses_and_erase(rewriter, op, &replacement, 1));
  *out_result = (loom_target_legalizer_result_t){
      .action = LOOM_TARGET_LEGALIZER_ACTION_REWRITTEN,
  };
  return iree_ok_status();
}

static bool loom_conversion_match_integer_to_narrow_float(
    const loom_target_legalizer_entry_t* entry,
    const loom_target_legalization_context_t* context, const loom_op_t* op) {
  (void)entry;
  const loom_scalar_type_t result_element = loom_type_element_type(
      loom_module_value_type(context->module, loom_op_results(op)[0]));
  // All integers near a finite F16/FP8 rounding boundary are exact in F32.
  // Larger magnitudes overflow IEEE formats or saturate E4M3 identically.
  if (result_element == LOOM_SCALAR_TYPE_F16 ||
      result_element == LOOM_SCALAR_TYPE_F8E4M3 ||
      result_element == LOOM_SCALAR_TYPE_F8E5M2) {
    return true;
  }
  // I1/I8/I16 are exact in F32. Wider integers are not: e.g. 16842753 rounds
  // to BF16 0x4b81 directly, but to 0x4b80 through F32. Keep those
  // conversions single-rounded.
  const loom_scalar_type_t input_element = loom_type_element_type(
      loom_module_value_type(context->module, loom_op_operands(op)[0]));
  return result_element == LOOM_SCALAR_TYPE_BF16 &&
         loom_scalar_type_set_contains(LOOM_SCALAR_TYPE_SET_I1 |
                                           LOOM_SCALAR_TYPE_SET_I8 |
                                           LOOM_SCALAR_TYPE_SET_I16,
                                       input_element);
}

static iree_status_t loom_conversion_legalize_integer_to_float(
    const loom_target_legalizer_entry_t* entry,
    loom_target_legalization_context_t* context, loom_op_t* op,
    loom_target_legalizer_result_t* out_result) {
  (void)entry;
  const bool is_signed =
      op->kind == LOOM_OP_SCALAR_SITOFP || op->kind == LOOM_OP_VECTOR_SITOFP;
  const bool is_vector = loom_type_is_vector(
      loom_module_value_type(context->module, loom_op_operands(op)[0]));
  loom_conversion_builder_fn_t convert =
      is_vector
          ? (is_signed ? loom_vector_sitofp_build : loom_vector_uitofp_build)
          : (is_signed ? loom_scalar_sitofp_build : loom_scalar_uitofp_build);
  return loom_conversion_stage_f32(
      context, op, convert,
      is_vector ? loom_vector_fptrunc_build : loom_scalar_fptrunc_build,
      out_result);
}

static iree_status_t loom_conversion_legalize_float_to_integer(
    const loom_target_legalizer_entry_t* entry,
    loom_target_legalization_context_t* context, loom_op_t* op,
    loom_target_legalizer_result_t* out_result) {
  (void)entry;
  const bool is_signed =
      op->kind == LOOM_OP_SCALAR_FPTOSI || op->kind == LOOM_OP_VECTOR_FPTOSI;
  const bool is_vector = loom_type_is_vector(
      loom_module_value_type(context->module, loom_op_operands(op)[0]));
  loom_conversion_builder_fn_t convert =
      is_vector
          ? (is_signed ? loom_vector_fptosi_build : loom_vector_fptoui_build)
          : (is_signed ? loom_scalar_fptosi_build : loom_scalar_fptoui_build);
  // Every F16/BF16/FP8 source value widens exactly, so the original toward-zero
  // conversion and its defined integer result domain are unchanged.
  return loom_conversion_stage_f32(
      context, op, is_vector ? loom_vector_extf_build : loom_scalar_extf_build,
      convert, out_result);
}

static const loom_target_legalizer_rule_t kConversionLegalizerRules[] = {
    {
        .flags = LOOM_TARGET_LEGALIZER_ENTRY_FLAG_REQUIRE_CONTRACT_REJECTION,
        .root_kind = LOOM_OP_SCALAR_SITOFP,
        .first_operand_element_types = LOOM_SCALAR_TYPE_SET_INTEGER,
        .match = loom_conversion_match_integer_to_narrow_float,
        .legalize = loom_conversion_legalize_integer_to_float,
    },
    {
        .flags = LOOM_TARGET_LEGALIZER_ENTRY_FLAG_REQUIRE_CONTRACT_REJECTION,
        .root_kind = LOOM_OP_SCALAR_UITOFP,
        .first_operand_element_types = LOOM_SCALAR_TYPE_SET_INTEGER,
        .match = loom_conversion_match_integer_to_narrow_float,
        .legalize = loom_conversion_legalize_integer_to_float,
    },
    {
        .flags = LOOM_TARGET_LEGALIZER_ENTRY_FLAG_REQUIRE_CONTRACT_REJECTION,
        .root_kind = LOOM_OP_VECTOR_SITOFP,
        .first_operand_element_types = LOOM_SCALAR_TYPE_SET_INTEGER,
        .match = loom_conversion_match_integer_to_narrow_float,
        .legalize = loom_conversion_legalize_integer_to_float,
    },
    {
        .flags = LOOM_TARGET_LEGALIZER_ENTRY_FLAG_REQUIRE_CONTRACT_REJECTION,
        .root_kind = LOOM_OP_VECTOR_UITOFP,
        .first_operand_element_types = LOOM_SCALAR_TYPE_SET_INTEGER,
        .match = loom_conversion_match_integer_to_narrow_float,
        .legalize = loom_conversion_legalize_integer_to_float,
    },
    {
        .flags = LOOM_TARGET_LEGALIZER_ENTRY_FLAG_REQUIRE_CONTRACT_REJECTION,
        .root_kind = LOOM_OP_SCALAR_FPTOSI,
        .first_operand_element_types = LOOM_SCALAR_TYPE_SET_16BIT_FLOAT |
                                       LOOM_SCALAR_TYPE_SET_F8E4M3 |
                                       LOOM_SCALAR_TYPE_SET_F8E5M2,
        .legalize = loom_conversion_legalize_float_to_integer,
    },
    {
        .flags = LOOM_TARGET_LEGALIZER_ENTRY_FLAG_REQUIRE_CONTRACT_REJECTION,
        .root_kind = LOOM_OP_SCALAR_FPTOUI,
        .first_operand_element_types = LOOM_SCALAR_TYPE_SET_16BIT_FLOAT |
                                       LOOM_SCALAR_TYPE_SET_F8E4M3 |
                                       LOOM_SCALAR_TYPE_SET_F8E5M2,
        .legalize = loom_conversion_legalize_float_to_integer,
    },
    {
        .flags = LOOM_TARGET_LEGALIZER_ENTRY_FLAG_REQUIRE_CONTRACT_REJECTION,
        .root_kind = LOOM_OP_VECTOR_FPTOSI,
        .first_operand_element_types = LOOM_SCALAR_TYPE_SET_16BIT_FLOAT |
                                       LOOM_SCALAR_TYPE_SET_F8E4M3 |
                                       LOOM_SCALAR_TYPE_SET_F8E5M2,
        .legalize = loom_conversion_legalize_float_to_integer,
    },
    {
        .flags = LOOM_TARGET_LEGALIZER_ENTRY_FLAG_REQUIRE_CONTRACT_REJECTION,
        .root_kind = LOOM_OP_VECTOR_FPTOUI,
        .first_operand_element_types = LOOM_SCALAR_TYPE_SET_16BIT_FLOAT |
                                       LOOM_SCALAR_TYPE_SET_F8E4M3 |
                                       LOOM_SCALAR_TYPE_SET_F8E5M2,
        .legalize = loom_conversion_legalize_float_to_integer,
    },
};

static const loom_target_legalizer_provider_t kConversionLegalizerProvider = {
    .name = IREE_SVL("conversion"),
    .strategy = LOOM_TARGET_LEGALIZER_STRATEGY_REFERENCE,
    .rules = kConversionLegalizerRules,
    .rule_count = IREE_ARRAYSIZE(kConversionLegalizerRules),
};

const loom_target_legalizer_provider_t*
loom_conversion_target_legalizer_provider(void) {
  return &kConversionLegalizerProvider;
}
