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
#include "loom/transforms/scalar/target_legalization.h"
#include "loom/transforms/vector/to_scalar.h"

static bool loom_spirv_legalizer_descriptor_set_is_spirv(
    const loom_low_descriptor_set_t* descriptor_set) {
  return descriptor_set != NULL &&
         descriptor_set->target_stable_id ==
             loom_spirv_logical_core_descriptor_set()->target_stable_id;
}

static iree_status_t loom_spirv_legalize_vector_to_scalar(
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
  // Capture every lane at the source operation. For reads, delaying this
  // rewrite until an arithmetic consumer would change the snapshot across an
  // aliasing write. FP8 vector extension must also become scalar before SPIR-V
  // value mapping because the target has no ordinary FP8 vector representation.
  IREE_RETURN_IF_ERROR(loom_vector_descriptor_to_scalar_rewrite_op(
      context->pass, context->rewriter, op, &rewritten));
  if (rewritten) {
    out_result->action = LOOM_TARGET_LEGALIZER_ACTION_REWRITTEN;
  }
  return iree_ok_status();
}

static iree_status_t loom_spirv_legalize_float8_to_bfloat_extension(
    const loom_target_legalizer_entry_t* entry,
    loom_target_legalization_context_t* context, loom_op_t* op,
    loom_target_legalizer_result_t* out_result) {
  *out_result = (loom_target_legalizer_result_t){
      .action = LOOM_TARGET_LEGALIZER_ACTION_NO_COMMENT,
  };
  if (!loom_spirv_legalizer_descriptor_set_is_spirv(context->descriptor_set)) {
    return iree_ok_status();
  }
  (void)entry;
  IREE_RETURN_IF_ERROR(loom_scalar_rewrite_float8_extension(context, op));
  out_result->action = LOOM_TARGET_LEGALIZER_ACTION_REWRITTEN;
  return iree_ok_status();
}

static const loom_target_legalizer_rule_t kSpirvLegalizerRules[] = {
    {
        .root_kind = LOOM_OP_SCALAR_EXTF,
        .first_operand_element_types =
            LOOM_SCALAR_TYPE_SET_F8E4M3 | LOOM_SCALAR_TYPE_SET_F8E5M2,
        .match = loom_scalar_match_float8_to_bfloat_extension,
        .legalize = loom_spirv_legalize_float8_to_bfloat_extension,
    },
    {
        .root_kind = LOOM_OP_VECTOR_LOAD,
        .legalize = loom_spirv_legalize_vector_to_scalar,
    },
    {
        .root_kind = LOOM_OP_VECTOR_EXTF,
        .first_operand_element_types =
            LOOM_SCALAR_TYPE_SET_F8E4M3 | LOOM_SCALAR_TYPE_SET_F8E5M2,
        .legalize = loom_spirv_legalize_vector_to_scalar,
    },
    {
        .root_kind = LOOM_OP_VECTOR_BITCAST,
        .flags = LOOM_TARGET_LEGALIZER_ENTRY_FLAG_REQUIRE_CONTRACT_REJECTION,
        .legalize = loom_spirv_legalize_vector_to_scalar,
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
