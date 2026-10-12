// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/transforms/index/target_legalization.h"

#include "loom/ops/index/ops.h"
#include "loom/rewrite/rewriter.h"

static iree_status_t loom_index_legalize_divrem(
    const loom_target_legalizer_entry_t* entry,
    loom_target_legalization_context_t* context, loom_op_t* op,
    loom_target_legalizer_result_t* out_result) {
  (void)entry;
  loom_rewriter_t* rewriter = context->rewriter;
  loom_builder_t* builder = &rewriter->builder;
  loom_builder_set_before(builder, op);
  const loom_value_id_t checkpoint = loom_rewriter_value_checkpoint(rewriter);
  const loom_value_id_t lhs = loom_index_divrem_lhs(op);
  const loom_value_id_t rhs = loom_index_divrem_rhs(op);
  loom_op_t* quotient = NULL;
  IREE_RETURN_IF_ERROR(
      loom_index_div_build(builder, lhs, rhs, op->location, &quotient));
  loom_op_t* remainder = NULL;
  IREE_RETURN_IF_ERROR(
      loom_index_rem_build(builder, lhs, rhs, op->location, &remainder));
  const loom_value_id_t replacements[2] = {
      loom_index_div_result(quotient),
      loom_index_rem_result(remainder),
  };
  IREE_RETURN_IF_ERROR(loom_rewriter_preserve_result_names_on_new_values(
      rewriter, op, replacements, IREE_ARRAYSIZE(replacements), checkpoint));
  IREE_RETURN_IF_ERROR(loom_rewriter_replace_all_uses_and_erase(
      rewriter, op, replacements, IREE_ARRAYSIZE(replacements)));
  *out_result = (loom_target_legalizer_result_t){
      .action = LOOM_TARGET_LEGALIZER_ACTION_REWRITTEN,
  };
  return iree_ok_status();
}

static const loom_target_legalizer_rule_t kIndexLegalizerRules[] = {
    {
        .root_kind = LOOM_OP_INDEX_DIVREM,
        .legalize = loom_index_legalize_divrem,
    },
};

static const loom_target_legalizer_provider_t kIndexLegalizerProvider = {
    .name = IREE_SVL("index"),
    .strategy = LOOM_TARGET_LEGALIZER_STRATEGY_REFERENCE,
    .rules = kIndexLegalizerRules,
    .rule_count = IREE_ARRAYSIZE(kIndexLegalizerRules),
};

const loom_target_legalizer_provider_t* loom_index_target_legalizer_provider(
    void) {
  return &kIndexLegalizerProvider;
}
