// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/decision/predicate.h"
#include "loom/ops/sanitizer/ops.h"
#include "loom/rewrite/rewriter.h"

static bool loom_sanitizer_find_value(loom_value_slice_t values,
                                      loom_value_id_t value_id,
                                      uint16_t* out_ordinal) {
  for (uint16_t i = 0; i < values.count; ++i) {
    if (values.values[i] != value_id) {
      continue;
    }
    if (out_ordinal) {
      *out_ordinal = i;
    }
    return true;
  }
  return false;
}

static bool loom_sanitizer_predicate_arg_operand(
    const loom_predicate_t* predicate, uint8_t argument_index,
    loom_rewriter_t* rewriter, loom_value_slice_t values,
    loom_decision_predicate_operand_t* out_operand) {
  if (argument_index >= predicate->arg_count) {
    return false;
  }
  *out_operand = (loom_decision_predicate_operand_t){
      .facts = loom_value_facts_unknown(),
      .identity = LOOM_DECISION_OPERAND_IDENTITY_NONE,
  };
  switch ((loom_predicate_arg_tag_t)predicate->arg_tags[argument_index]) {
    case LOOM_PRED_ARG_CONST:
      out_operand->facts =
          loom_value_facts_exact_i64(predicate->args[argument_index]);
      return true;
    case LOOM_PRED_ARG_VALUE: {
      if (predicate->args[argument_index] < 0) {
        return false;
      }
      const loom_value_id_t value_id =
          (loom_value_id_t)predicate->args[argument_index];
      if (!loom_sanitizer_find_value(values, value_id, NULL)) {
        return false;
      }
      out_operand->facts = loom_rewriter_value_facts(rewriter, value_id);
      out_operand->identity = value_id;
      return true;
    }
    case LOOM_PRED_ARG_NONE:
    case LOOM_PRED_ARG_COUNT_:
      return false;
  }
  return false;
}

static bool loom_sanitizer_predicate_is_proven(
    const loom_predicate_t* predicate, loom_rewriter_t* rewriter,
    loom_value_slice_t values) {
  loom_decision_predicate_operand_t operands[3] = {0};
  for (uint8_t i = 0; i < predicate->arg_count; ++i) {
    if (!loom_sanitizer_predicate_arg_operand(predicate, i, rewriter, values,
                                              &operands[i])) {
      return false;
    }
  }
  return loom_decision_predicate_evaluate(predicate->kind, operands) ==
         LOOM_DECISION_TRUTH_TRUE;
}

static bool loom_sanitizer_predicate_list_is_proven(loom_attribute_t predicates,
                                                    loom_rewriter_t* rewriter,
                                                    loom_value_slice_t values) {
  if (predicates.kind != LOOM_ATTR_PREDICATE_LIST) {
    return false;
  }
  for (uint16_t i = 0; i < predicates.count; ++i) {
    if (!loom_sanitizer_predicate_is_proven(&predicates.predicate_list[i],
                                            rewriter, values)) {
      return false;
    }
  }
  return true;
}

iree_status_t loom_sanitizer_assert_value_canonicalize(
    loom_op_t* op, loom_rewriter_t* rewriter) {
  loom_value_slice_t values = loom_sanitizer_assert_value_values(op);
  loom_value_slice_t results = loom_sanitizer_assert_value_results(op);
  if (values.count != results.count) {
    return iree_ok_status();
  }
  if (!loom_sanitizer_predicate_list_is_proven(
          loom_sanitizer_assert_value_predicates(op), rewriter, values)) {
    return iree_ok_status();
  }
  return loom_rewriter_replace_all_uses_and_erase(rewriter, op, values.values,
                                                  values.count);
}

iree_status_t loom_sanitizer_assert_op_canonicalize(loom_op_t* op,
                                                    loom_rewriter_t* rewriter) {
  if (!loom_sanitizer_predicate_list_is_proven(
          loom_sanitizer_assert_op_predicates(op), rewriter,
          loom_sanitizer_assert_op_values(op))) {
    return iree_ok_status();
  }
  return loom_rewriter_erase(rewriter, op);
}
