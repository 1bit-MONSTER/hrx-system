// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/ir/float_facts.h"
#include "loom/ir/module.h"
#include "loom/ops/op_defs.h"
#include "loom/ops/scalar/compare.h"
#include "loom/ops/scalar/ops.h"
#include "loom/rewrite/rewriter.h"

static loom_type_t loom_scalar_single_result_type(loom_rewriter_t* rewriter,
                                                  const loom_op_t* op) {
  return loom_module_value_type(rewriter->module, loom_op_const_results(op)[0]);
}

static bool loom_scalar_query_exact_i64(loom_rewriter_t* rewriter,
                                        loom_value_id_t value_id,
                                        int64_t* out_value) {
  loom_value_facts_t facts = loom_rewriter_value_facts(rewriter, value_id);
  if (!loom_value_facts_is_exact(facts) || loom_value_facts_is_float(facts)) {
    return false;
  }
  *out_value = facts.range_lo;
  return true;
}

static bool loom_scalar_value_facts_are_exact_i64(loom_rewriter_t* rewriter,
                                                  loom_value_id_t value_id,
                                                  int64_t expected_value) {
  int64_t actual_value = 0;
  return loom_scalar_query_exact_i64(rewriter, value_id, &actual_value) &&
         actual_value == expected_value;
}

static bool loom_scalar_query_exact_float(loom_rewriter_t* rewriter,
                                          loom_value_id_t value_id,
                                          double* out_value) {
  loom_value_facts_t facts = loom_rewriter_value_facts(rewriter, value_id);
  loom_type_t type = loom_module_value_type(rewriter->module, value_id);
  return loom_type_is_scalar(type) &&
         loom_value_facts_as_exact_float(loom_type_element_type(type), facts,
                                         out_value);
}

static iree_status_t loom_scalar_replace_single_result_with_value(
    loom_op_t* op, loom_rewriter_t* rewriter, loom_value_id_t replacement) {
  return loom_rewriter_replace_all_uses_and_erase(rewriter, op, &replacement,
                                                  1);
}

static iree_status_t loom_scalar_replace_single_result_with_i64_constant(
    loom_op_t* op, loom_rewriter_t* rewriter, int64_t value) {
  loom_builder_set_before(&rewriter->builder, op);
  loom_value_id_t value_checkpoint = loom_rewriter_value_checkpoint(rewriter);
  loom_value_id_t replacement = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(
      loom_rewriter_build_constant(rewriter, loom_value_facts_exact_i64(value),
                                   loom_scalar_single_result_type(rewriter, op),
                                   op->location, &replacement));
  IREE_RETURN_IF_ERROR(loom_rewriter_preserve_result_names_on_new_values(
      rewriter, op, &replacement, 1, value_checkpoint));
  return loom_scalar_replace_single_result_with_value(op, rewriter,
                                                      replacement);
}

static iree_status_t loom_scalar_replace_single_result_with_cmpi(
    loom_op_t* op, loom_rewriter_t* rewriter, uint8_t predicate,
    loom_value_id_t lhs, loom_value_id_t rhs) {
  loom_builder_set_before(&rewriter->builder, op);
  loom_value_id_t value_checkpoint = loom_rewriter_value_checkpoint(rewriter);

  loom_op_t* replacement_op = NULL;
  IREE_RETURN_IF_ERROR(loom_scalar_cmpi_build(
      &rewriter->builder, predicate, lhs, rhs, op->location, &replacement_op));
  loom_value_id_t replacement = loom_scalar_cmpi_result(replacement_op);
  IREE_RETURN_IF_ERROR(loom_rewriter_preserve_result_names_on_new_values(
      rewriter, op, &replacement, 1, value_checkpoint));
  return loom_scalar_replace_single_result_with_value(op, rewriter,
                                                      replacement);
}

//===----------------------------------------------------------------------===//
// Comparison
//===----------------------------------------------------------------------===//

static iree_status_t loom_scalar_cmpi_unsigned_zero_canonicalize(
    loom_op_t* op, loom_rewriter_t* rewriter, uint8_t predicate,
    loom_value_id_t lhs, loom_value_id_t rhs, bool* out_changed) {
  *out_changed = true;
  if (loom_scalar_value_facts_are_exact_i64(rewriter, rhs, 0)) {
    switch ((loom_scalar_cmpi_predicate_t)predicate) {
      case LOOM_SCALAR_CMPI_PREDICATE_ULT:
        return loom_scalar_replace_single_result_with_i64_constant(op, rewriter,
                                                                   0);
      case LOOM_SCALAR_CMPI_PREDICATE_UGE:
        return loom_scalar_replace_single_result_with_i64_constant(op, rewriter,
                                                                   1);
      case LOOM_SCALAR_CMPI_PREDICATE_ULE:
        return loom_scalar_replace_single_result_with_cmpi(
            op, rewriter, LOOM_SCALAR_CMPI_PREDICATE_EQ, lhs, rhs);
      case LOOM_SCALAR_CMPI_PREDICATE_UGT:
        return loom_scalar_replace_single_result_with_cmpi(
            op, rewriter, LOOM_SCALAR_CMPI_PREDICATE_NE, lhs, rhs);
      default:
        break;
    }
  }
  if (loom_scalar_value_facts_are_exact_i64(rewriter, lhs, 0)) {
    switch ((loom_scalar_cmpi_predicate_t)predicate) {
      case LOOM_SCALAR_CMPI_PREDICATE_ULE:
        return loom_scalar_replace_single_result_with_i64_constant(op, rewriter,
                                                                   1);
      case LOOM_SCALAR_CMPI_PREDICATE_UGT:
        return loom_scalar_replace_single_result_with_i64_constant(op, rewriter,
                                                                   0);
      case LOOM_SCALAR_CMPI_PREDICATE_ULT:
        return loom_scalar_replace_single_result_with_cmpi(
            op, rewriter, LOOM_SCALAR_CMPI_PREDICATE_NE, rhs, lhs);
      case LOOM_SCALAR_CMPI_PREDICATE_UGE:
        return loom_scalar_replace_single_result_with_cmpi(
            op, rewriter, LOOM_SCALAR_CMPI_PREDICATE_EQ, rhs, lhs);
      default:
        break;
    }
  }
  *out_changed = false;
  return iree_ok_status();
}

// Boolean comparisons have a complete logical representation without integer
// extension. Signed predicates reverse the zero/one ordering because the set
// bit represents -1 in the signed one-bit domain.
static iree_status_t loom_scalar_cmpi_boolean_canonicalize(
    loom_op_t* op, loom_rewriter_t* rewriter, uint8_t predicate,
    loom_value_id_t lhs, loom_value_id_t rhs) {
  loom_builder_t* builder = &rewriter->builder;
  loom_builder_set_before(builder, op);
  const loom_value_id_t checkpoint = loom_rewriter_value_checkpoint(rewriter);
  const loom_type_t type = loom_type_scalar(LOOM_SCALAR_TYPE_I1);
  loom_op_t* replacement_op = NULL;
  if (predicate == LOOM_SCALAR_CMPI_PREDICATE_EQ ||
      predicate == LOOM_SCALAR_CMPI_PREDICATE_NE) {
    IREE_RETURN_IF_ERROR(loom_scalar_xori_build(builder, lhs, rhs, type,
                                                op->location, &replacement_op));
    if (predicate == LOOM_SCALAR_CMPI_PREDICATE_EQ) {
      loom_value_id_t one = LOOM_VALUE_ID_INVALID;
      IREE_RETURN_IF_ERROR(loom_rewriter_build_constant(
          rewriter, loom_value_facts_exact_i64(1), type, op->location, &one));
      IREE_RETURN_IF_ERROR(loom_scalar_xori_build(
          builder, loom_scalar_xori_result(replacement_op), one, type,
          op->location, &replacement_op));
    }
  } else {
    predicate =
        loom_scalar_cmpi_range_predicate(LOOM_SCALAR_TYPE_I1, predicate);
    // Ascending order inverts lhs; descending order inverts rhs. Strict order
    // uses AND, inclusive order uses OR.
    if (predicate == LOOM_SCALAR_CMPI_PREDICATE_SGT ||
        predicate == LOOM_SCALAR_CMPI_PREDICATE_SGE ||
        predicate == LOOM_SCALAR_CMPI_PREDICATE_UGT ||
        predicate == LOOM_SCALAR_CMPI_PREDICATE_UGE) {
      loom_value_id_t swap = lhs;
      lhs = rhs;
      rhs = swap;
    }
    loom_value_id_t one = LOOM_VALUE_ID_INVALID;
    IREE_RETURN_IF_ERROR(loom_rewriter_build_constant(
        rewriter, loom_value_facts_exact_i64(1), type, op->location, &one));
    loom_op_t* inverted = NULL;
    IREE_RETURN_IF_ERROR(loom_scalar_xori_build(builder, lhs, one, type,
                                                op->location, &inverted));
    if (predicate == LOOM_SCALAR_CMPI_PREDICATE_SLT ||
        predicate == LOOM_SCALAR_CMPI_PREDICATE_SGT ||
        predicate == LOOM_SCALAR_CMPI_PREDICATE_ULT ||
        predicate == LOOM_SCALAR_CMPI_PREDICATE_UGT) {
      IREE_RETURN_IF_ERROR(
          loom_scalar_andi_build(builder, loom_scalar_xori_result(inverted),
                                 rhs, type, op->location, &replacement_op));
    } else {
      IREE_RETURN_IF_ERROR(
          loom_scalar_ori_build(builder, loom_scalar_xori_result(inverted), rhs,
                                type, op->location, &replacement_op));
    }
  }
  loom_value_id_t replacement = loom_op_results(replacement_op)[0];
  IREE_RETURN_IF_ERROR(loom_rewriter_preserve_result_names_on_new_values(
      rewriter, op, &replacement, 1, checkpoint));
  return loom_scalar_replace_single_result_with_value(op, rewriter,
                                                      replacement);
}

// Both integer extensions preserve whether a predicate is zero. Only the
// immediate producer participates; other integer values retain their test.
static loom_value_id_t loom_scalar_extended_boolean_input(
    const loom_rewriter_t* rewriter, loom_value_id_t value_id) {
  const loom_value_t* value = loom_module_value(rewriter->module, value_id);
  if (loom_value_is_block_arg(value)) {
    return LOOM_VALUE_ID_INVALID;
  }
  const loom_op_t* producer = loom_value_def_op(value);
  if (!loom_scalar_extui_isa(producer) && !loom_scalar_extsi_isa(producer)) {
    return LOOM_VALUE_ID_INVALID;
  }
  const loom_value_id_t input = loom_op_const_operands(producer)[0];
  return loom_type_element_type(loom_module_value_type(
             rewriter->module, input)) == LOOM_SCALAR_TYPE_I1
             ? input
             : LOOM_VALUE_ID_INVALID;
}

iree_status_t loom_scalar_cmpi_canonicalize(loom_op_t* op,
                                            loom_rewriter_t* rewriter) {
  loom_value_id_t lhs = loom_scalar_cmpi_lhs(op);
  loom_value_id_t rhs = loom_scalar_cmpi_rhs(op);
  uint8_t predicate = loom_scalar_cmpi_predicate(op);
  loom_type_t operand_type = loom_module_value_type(rewriter->module, lhs);

  bool result = false;
  loom_value_facts_t lhs_facts = loom_rewriter_value_facts(rewriter, lhs);
  loom_value_facts_t rhs_facts = loom_rewriter_value_facts(rewriter, rhs);
  if ((lhs == rhs && loom_scalar_cmpi_same_value_result(predicate, &result)) ||
      loom_scalar_cmpi_result_from_facts(loom_type_element_type(operand_type),
                                         predicate, &lhs_facts, &rhs_facts,
                                         &result)) {
    return loom_scalar_replace_single_result_with_i64_constant(op, rewriter,
                                                               result ? 1 : 0);
  }

  if (loom_type_element_type(operand_type) == LOOM_SCALAR_TYPE_I1) {
    return loom_scalar_cmpi_boolean_canonicalize(op, rewriter, predicate, lhs,
                                                 rhs);
  }
  if ((predicate == LOOM_SCALAR_CMPI_PREDICATE_EQ ||
       predicate == LOOM_SCALAR_CMPI_PREDICATE_NE) &&
      loom_value_facts_is_exact(rhs_facts) && rhs_facts.range_lo == 0) {
    loom_value_id_t input = loom_scalar_extended_boolean_input(rewriter, lhs);
    if (input != LOOM_VALUE_ID_INVALID) {
      loom_builder_set_before(&rewriter->builder, op);
      if (predicate == LOOM_SCALAR_CMPI_PREDICATE_EQ) {
        const loom_value_id_t checkpoint =
            loom_rewriter_value_checkpoint(rewriter);
        const loom_type_t type = loom_type_scalar(LOOM_SCALAR_TYPE_I1);
        loom_value_id_t one = LOOM_VALUE_ID_INVALID;
        IREE_RETURN_IF_ERROR(loom_rewriter_build_constant(
            rewriter, loom_value_facts_exact_i64(1), type, op->location, &one));
        loom_op_t* inverted = NULL;
        IREE_RETURN_IF_ERROR(loom_scalar_xori_build(
            &rewriter->builder, input, one, type, op->location, &inverted));
        input = loom_scalar_xori_result(inverted);
        IREE_RETURN_IF_ERROR(loom_rewriter_preserve_result_names_on_new_values(
            rewriter, op, &input, 1, checkpoint));
      }
      return loom_scalar_replace_single_result_with_value(op, rewriter, input);
    }
  }
  bool changed = false;
  IREE_RETURN_IF_ERROR(loom_scalar_cmpi_unsigned_zero_canonicalize(
      op, rewriter, predicate, lhs, rhs, &changed));
  if (changed) {
    return iree_ok_status();
  }

  int64_t lhs_value = 0;
  int64_t rhs_value = 0;
  if (loom_scalar_query_exact_i64(rewriter, lhs, &lhs_value) &&
      !loom_scalar_query_exact_i64(rewriter, rhs, &rhs_value)) {
    return loom_scalar_replace_single_result_with_cmpi(
        op, rewriter, loom_scalar_cmpi_swapped_predicate(predicate), rhs, lhs);
  }
  return iree_ok_status();
}

iree_status_t loom_scalar_cmpf_canonicalize(loom_op_t* op,
                                            loom_rewriter_t* rewriter) {
  loom_value_id_t lhs = loom_scalar_cmpf_lhs(op);
  loom_value_id_t rhs = loom_scalar_cmpf_rhs(op);
  uint8_t predicate = loom_scalar_cmpf_predicate(op);

  bool result = false;
  if (loom_scalar_cmpf_constant_result(
          predicate, lhs, rhs, loom_scalar_cmpf_fastmath(op), &result)) {
    return loom_scalar_replace_single_result_with_i64_constant(op, rewriter,
                                                               result ? 1 : 0);
  }

  double lhs_value = 0.0;
  double rhs_value = 0.0;
  if (loom_scalar_query_exact_float(rewriter, lhs, &lhs_value) &&
      loom_scalar_query_exact_float(rewriter, rhs, &rhs_value) &&
      loom_scalar_cmpf_exact_result(predicate, lhs_value, rhs_value, &result)) {
    return loom_scalar_replace_single_result_with_i64_constant(op, rewriter,
                                                               result ? 1 : 0);
  }
  return iree_ok_status();
}
