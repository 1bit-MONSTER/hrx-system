// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/target/arch/amdgpu/lower/value/dynamic_integer_division.h"

#include "loom/ir/context.h"
#include "loom/ops/index/ops.h"
#include "loom/ops/scalar/ops.h"
#include "loom/target/arch/amdgpu/lower/descriptor_ref.h"
#include "loom/target/arch/amdgpu/lower/emit.h"
#include "loom/target/arch/amdgpu/lower/materializers.h"
#include "loom/target/arch/amdgpu/lower/types.h"
#include "loom/util/fact_table.h"

typedef enum loom_amdgpu_integer_division_domain_e {
  LOOM_AMDGPU_INTEGER_DIVISION_DOMAIN_UNSIGNED = 0,
  LOOM_AMDGPU_INTEGER_DIVISION_DOMAIN_SIGNED = 1,
} loom_amdgpu_integer_division_domain_t;

static loom_amdgpu_integer_division_domain_t
loom_amdgpu_integer_division_domain(const loom_op_t* op) {
  return loom_scalar_divsi_isa(op) || loom_scalar_remsi_isa(op) ||
                 loom_scalar_divremsi_isa(op)
             ? LOOM_AMDGPU_INTEGER_DIVISION_DOMAIN_SIGNED
             : LOOM_AMDGPU_INTEGER_DIVISION_DOMAIN_UNSIGNED;
}

static bool loom_amdgpu_exact_i64(const loom_value_fact_table_t* facts,
                                  loom_value_id_t value_id,
                                  int64_t* out_value) {
  return facts && loom_value_facts_as_exact_i64(
                      loom_value_fact_table_lookup(facts, value_id), out_value);
}

static void loom_amdgpu_dynamic_u32_division_results(
    const loom_op_t* op, loom_value_id_t* out_quotient,
    loom_value_id_t* out_remainder) {
  *out_quotient = LOOM_VALUE_ID_INVALID;
  *out_remainder = LOOM_VALUE_ID_INVALID;
  const loom_value_id_t* results = loom_op_const_results(op);
  switch (op->kind) {
    case LOOM_OP_SCALAR_DIVSI:
    case LOOM_OP_SCALAR_DIVUI:
    case LOOM_OP_INDEX_DIV:
      *out_quotient = results[0];
      return;
    case LOOM_OP_SCALAR_REMSI:
    case LOOM_OP_SCALAR_REMUI:
    case LOOM_OP_INDEX_REM:
      *out_remainder = results[0];
      return;
    case LOOM_OP_SCALAR_DIVREMSI:
    case LOOM_OP_SCALAR_DIVREMUI:
    case LOOM_OP_INDEX_DIVREM:
      *out_quotient = results[0];
      *out_remainder = results[1];
      return;
    default:
      IREE_ASSERT_UNREACHABLE("unexpected runtime integer division operation");
      IREE_BUILTIN_UNREACHABLE();
  }
}

static bool loom_amdgpu_u32_division_descriptors_supported(
    const loom_low_descriptor_set_t* descriptors, uint16_t register_class_id,
    loom_amdgpu_integer_division_domain_t domain) {
  static const loom_amdgpu_descriptor_ref_t common[] = {
      LOOM_AMDGPU_DESCRIPTOR_REF_V_CVT_F32_U32,
      LOOM_AMDGPU_DESCRIPTOR_REF_V_RCP_F32,
      LOOM_AMDGPU_DESCRIPTOR_REF_V_MUL_F32_LIT,
      LOOM_AMDGPU_DESCRIPTOR_REF_V_CVT_U32_F32,
  };
  static const loom_amdgpu_descriptor_ref_t sgpr[] = {
      LOOM_AMDGPU_DESCRIPTOR_REF_V_READFIRSTLANE_B32,
      LOOM_AMDGPU_DESCRIPTOR_REF_S_MOV_B32,
      LOOM_AMDGPU_DESCRIPTOR_REF_S_MUL_I32,
      LOOM_AMDGPU_DESCRIPTOR_REF_S_MUL_HI_U32,
      LOOM_AMDGPU_DESCRIPTOR_REF_S_ADD_U32,
      LOOM_AMDGPU_DESCRIPTOR_REF_S_SUB_U32,
      LOOM_AMDGPU_DESCRIPTOR_REF_S_SUB_CO_U32,
      LOOM_AMDGPU_DESCRIPTOR_REF_S_CSELECT_B32,
  };
  static const loom_amdgpu_descriptor_ref_t vgpr[] = {
      LOOM_AMDGPU_DESCRIPTOR_REF_V_MOV_B32,
      LOOM_AMDGPU_DESCRIPTOR_REF_V_MUL_LO_U32,
      LOOM_AMDGPU_DESCRIPTOR_REF_V_MUL_HI_U32,
      LOOM_AMDGPU_DESCRIPTOR_REF_V_ADD_U32,
      LOOM_AMDGPU_DESCRIPTOR_REF_V_SUB_U32,
      LOOM_AMDGPU_DESCRIPTOR_REF_V_SUB_CO_U32,
      LOOM_AMDGPU_DESCRIPTOR_REF_V_CNDMASK_B32,
  };
  static const loom_amdgpu_descriptor_ref_t signed_sgpr[] = {
      LOOM_AMDGPU_DESCRIPTOR_REF_S_XOR_B32,
      LOOM_AMDGPU_DESCRIPTOR_REF_S_ASHR_I32,
  };
  static const loom_amdgpu_descriptor_ref_t signed_vgpr[] = {
      LOOM_AMDGPU_DESCRIPTOR_REF_V_XOR_B32,
      LOOM_AMDGPU_DESCRIPTOR_REF_V_ASHRREV_I32_SRC0_INLINE,
  };
  const bool uniform = register_class_id == LOOM_AMDGPU_REG_CLASS_ID_SGPR;
  if (!loom_amdgpu_descriptor_set_has_all_refs(descriptors, common,
                                               IREE_ARRAYSIZE(common)) ||
      !loom_amdgpu_descriptor_set_has_all_refs(
          descriptors, uniform ? sgpr : vgpr,
          uniform ? IREE_ARRAYSIZE(sgpr) : IREE_ARRAYSIZE(vgpr))) {
    return false;
  }
  return domain == LOOM_AMDGPU_INTEGER_DIVISION_DOMAIN_UNSIGNED ||
         loom_amdgpu_descriptor_set_has_all_refs(
             descriptors, uniform ? signed_sgpr : signed_vgpr,
             uniform ? IREE_ARRAYSIZE(signed_sgpr)
                     : IREE_ARRAYSIZE(signed_vgpr));
}

static bool loom_amdgpu_dynamic_u32_division_op(const loom_module_t* module,
                                                const loom_op_t* op) {
  const loom_value_id_t first_result = loom_op_const_results(op)[0];
  const loom_scalar_type_t element_type =
      loom_type_element_type(loom_module_value_type(module, first_result));
  return element_type == LOOM_SCALAR_TYPE_I32 ||
         element_type == LOOM_SCALAR_TYPE_INDEX;
}

static bool loom_amdgpu_dynamic_index_division_facts_supported(
    const loom_value_fact_table_t* fact_table, loom_value_id_t lhs,
    loom_value_id_t rhs, loom_value_id_t quotient, loom_value_id_t remainder) {
  if (!fact_table || !loom_value_facts_fit_unsigned_bit_count(
                         loom_value_fact_table_lookup(fact_table, lhs), 32)) {
    return false;
  }
  const loom_value_facts_t rhs_facts =
      loom_value_fact_table_lookup(fact_table, rhs);
  if (!loom_value_facts_is_positive(rhs_facts) ||
      !loom_value_facts_fit_unsigned_bit_count(rhs_facts, 32)) {
    return false;
  }
  if (quotient != LOOM_VALUE_ID_INVALID &&
      !loom_value_facts_fit_unsigned_bit_count(
          loom_value_fact_table_lookup(fact_table, quotient), 32)) {
    return false;
  }
  return remainder == LOOM_VALUE_ID_INVALID ||
         loom_value_facts_fit_unsigned_bit_count(
             loom_value_fact_table_lookup(fact_table, remainder), 32);
}

iree_status_t loom_amdgpu_select_dynamic_u32_division_plan(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    loom_amdgpu_dynamic_u32_division_plan_t* out_plan, bool* out_selected) {
  *out_plan = (loom_amdgpu_dynamic_u32_division_plan_t){
      .lhs = LOOM_VALUE_ID_INVALID,
      .rhs = LOOM_VALUE_ID_INVALID,
      .quotient = LOOM_VALUE_ID_INVALID,
      .remainder = LOOM_VALUE_ID_INVALID,
  };
  *out_selected = false;
  const loom_module_t* module = loom_low_lower_context_module(context);
  if (!loom_amdgpu_dynamic_u32_division_op(module, source_op)) {
    return iree_ok_status();
  }
  const loom_value_id_t* operands = loom_op_const_operands(source_op);
  int64_t exact_divisor = 0;
  if (loom_amdgpu_exact_i64(loom_low_lower_context_fact_table(context),
                            operands[1], &exact_divisor)) {
    return iree_ok_status();
  }

  loom_value_id_t quotient = LOOM_VALUE_ID_INVALID;
  loom_value_id_t remainder = LOOM_VALUE_ID_INVALID;
  loom_amdgpu_dynamic_u32_division_results(source_op, &quotient, &remainder);
  const bool is_index = loom_type_element_type(loom_module_value_type(
                            module, loom_op_const_results(source_op)[0])) ==
                        LOOM_SCALAR_TYPE_INDEX;
  if (is_index && !loom_amdgpu_dynamic_index_division_facts_supported(
                      loom_low_lower_context_fact_table(context), operands[0],
                      operands[1], quotient, remainder)) {
    return iree_ok_status();
  }
  uint16_t register_class_id = LOOM_AMDGPU_REG_CLASS_ID_SGPR;
  if (is_index) {
    const loom_value_id_t results[] = {quotient, remainder};
    uint16_t selected_result_class_id = 0;
    bool result_class_selected = false;
    for (uint32_t i = 0; i < IREE_ARRAYSIZE(results); ++i) {
      if (results[i] == LOOM_VALUE_ID_INVALID) {
        continue;
      }
      loom_type_t low_type = loom_type_none();
      IREE_RETURN_IF_ERROR(loom_amdgpu_low_result_type(context, source_op,
                                                       results[i], &low_type));
      uint16_t result_class_id = 0;
      if (loom_amdgpu_low_type_is_register_class_count(
              context, low_type, LOOM_AMDGPU_REG_CLASS_ID_SGPR, 1)) {
        result_class_id = LOOM_AMDGPU_REG_CLASS_ID_SGPR;
      } else if (loom_amdgpu_low_type_is_register_class_count(
                     context, low_type, LOOM_AMDGPU_REG_CLASS_ID_VGPR, 1)) {
        result_class_id = LOOM_AMDGPU_REG_CLASS_ID_VGPR;
      } else {
        return iree_ok_status();
      }
      if (result_class_selected &&
          selected_result_class_id != result_class_id) {
        return iree_ok_status();
      }
      selected_result_class_id = result_class_id;
      result_class_selected = true;
    }
    register_class_id = selected_result_class_id;
    for (uint32_t i = 0; i < 2; ++i) {
      bool can_materialize = false;
      if (register_class_id == LOOM_AMDGPU_REG_CLASS_ID_SGPR) {
        IREE_RETURN_IF_ERROR(loom_amdgpu_value_can_materialize_as_sgpr_address(
            context, source_op, operands[i], &can_materialize));
      } else {
        IREE_RETURN_IF_ERROR(loom_amdgpu_value_can_materialize_as_vgpr_address(
            context, source_op, operands[i], &can_materialize));
      }
      if (!can_materialize) {
        return iree_ok_status();
      }
    }
  } else {
    const loom_value_id_t values[] = {operands[0], operands[1], quotient,
                                      remainder};
    for (uint32_t i = 0; i < IREE_ARRAYSIZE(values); ++i) {
      if (values[i] == LOOM_VALUE_ID_INVALID) {
        continue;
      }
      loom_type_t low_type = loom_type_none();
      if (i < 2) {
        IREE_RETURN_IF_ERROR(
            loom_low_lower_map_value(context, source_op, values[i], &low_type));
      } else {
        IREE_RETURN_IF_ERROR(loom_amdgpu_low_result_type(context, source_op,
                                                         values[i], &low_type));
      }
      if (loom_amdgpu_low_type_is_register_class_count(
              context, low_type, LOOM_AMDGPU_REG_CLASS_ID_VGPR, 1)) {
        register_class_id = LOOM_AMDGPU_REG_CLASS_ID_VGPR;
      } else if (!loom_amdgpu_low_type_is_register_class_count(
                     context, low_type, LOOM_AMDGPU_REG_CLASS_ID_SGPR, 1)) {
        return iree_ok_status();
      }
    }
  }
  const loom_amdgpu_integer_division_domain_t domain =
      loom_amdgpu_integer_division_domain(source_op);
  if (!loom_amdgpu_u32_division_descriptors_supported(
          loom_low_lower_context_descriptor_set(context), register_class_id,
          domain)) {
    return iree_ok_status();
  }

  *out_plan = (loom_amdgpu_dynamic_u32_division_plan_t){
      .lhs = operands[0],
      .rhs = operands[1],
      .quotient = quotient,
      .remainder = remainder,
      .register_class_id = register_class_id,
  };
  *out_selected = true;
  return iree_ok_status();
}

iree_status_t loom_amdgpu_finalize_dynamic_u32_division_plan(
    loom_low_lower_context_t* context,
    const loom_amdgpu_dynamic_u32_division_plan_t* plan) {
  const loom_type_t result_type = loom_low_register_type(
      loom_low_lower_context_descriptor_set(context)->stable_id,
      plan->register_class_id, 1);
  if (plan->quotient != LOOM_VALUE_ID_INVALID) {
    IREE_RETURN_IF_ERROR(
        loom_low_lower_plan_value_type(context, plan->quotient, result_type));
  }
  if (plan->remainder != LOOM_VALUE_ID_INVALID) {
    IREE_RETURN_IF_ERROR(
        loom_low_lower_plan_value_type(context, plan->remainder, result_type));
  }
  return iree_ok_status();
}

void loom_amdgpu_mark_dynamic_u32_division_plan_storage_demands(
    loom_low_lower_context_t* context,
    const loom_amdgpu_dynamic_u32_division_plan_t* plan) {
  loom_low_lower_require_source_value_storage(context, plan->lhs);
  loom_low_lower_require_source_value_storage(context, plan->rhs);
}

static iree_status_t loom_amdgpu_dynamic_division_emit_constant(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    uint16_t register_class_id, uint32_t value, loom_type_t word_type,
    loom_value_id_t* out_value) {
  return loom_amdgpu_emit_const_u32(
      context, source_op,
      register_class_id == LOOM_AMDGPU_REG_CLASS_ID_SGPR
          ? LOOM_AMDGPU_DESCRIPTOR_REF_S_MOV_B32
          : LOOM_AMDGPU_DESCRIPTOR_REF_V_MOV_B32,
      value, word_type, out_value);
}

static iree_status_t loom_amdgpu_dynamic_division_emit_binary(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    uint16_t register_class_id, loom_amdgpu_descriptor_ref_t sgpr_descriptor,
    loom_amdgpu_descriptor_ref_t vgpr_descriptor, loom_value_id_t lhs,
    loom_value_id_t rhs, loom_type_t word_type, loom_value_id_t* out_value) {
  return loom_amdgpu_emit_binary(
      context, source_op,
      register_class_id == LOOM_AMDGPU_REG_CLASS_ID_SGPR ? sgpr_descriptor
                                                         : vgpr_descriptor,
      lhs, rhs, word_type, out_value);
}

static iree_status_t loom_amdgpu_dynamic_division_emit_sign(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    uint16_t register_class_id, loom_value_id_t value, loom_type_t word_type,
    loom_value_id_t* out_sign) {
  if (register_class_id == LOOM_AMDGPU_REG_CLASS_ID_SGPR) {
    return loom_amdgpu_emit_sgpr_binary_immediate(
        context, source_op, LOOM_AMDGPU_DESCRIPTOR_REF_S_ASHR_I32, value, 31,
        word_type, out_sign);
  }
  return loom_amdgpu_emit_vgpr_binary_immediate(
      context, source_op, LOOM_AMDGPU_DESCRIPTOR_REF_V_ASHRREV_I32_LIT, value,
      31, word_type, out_sign);
}

static iree_status_t loom_amdgpu_dynamic_division_emit_signed_magnitude(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    uint16_t register_class_id, loom_value_id_t value, loom_value_id_t sign,
    loom_type_t word_type, loom_value_id_t* out_magnitude) {
  loom_value_id_t sign_flipped = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_amdgpu_dynamic_division_emit_binary(
      context, source_op, register_class_id,
      LOOM_AMDGPU_DESCRIPTOR_REF_S_XOR_B32,
      LOOM_AMDGPU_DESCRIPTOR_REF_V_XOR_B32, value, sign, word_type,
      &sign_flipped));
  return loom_amdgpu_dynamic_division_emit_binary(
      context, source_op, register_class_id,
      LOOM_AMDGPU_DESCRIPTOR_REF_S_SUB_U32,
      LOOM_AMDGPU_DESCRIPTOR_REF_V_SUB_U32, sign_flipped, sign, word_type,
      out_magnitude);
}

static iree_status_t loom_amdgpu_dynamic_division_emit_reciprocal_seed(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    uint16_t register_class_id, loom_value_id_t divisor, loom_type_t word_type,
    loom_value_id_t* out_seed) {
  loom_type_t vgpr_type = loom_type_none();
  IREE_RETURN_IF_ERROR(loom_amdgpu_make_vgpr_type(context, &vgpr_type));
  loom_value_id_t divisor_float = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_amdgpu_emit_vgpr_unary(
      context, source_op, LOOM_AMDGPU_DESCRIPTOR_REF_V_CVT_F32_U32, divisor,
      vgpr_type, &divisor_float));
  loom_value_id_t reciprocal = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_amdgpu_emit_vgpr_unary(
      context, source_op, LOOM_AMDGPU_DESCRIPTOR_REF_V_RCP_F32, divisor_float,
      vgpr_type, &reciprocal));
  loom_value_id_t scaled_reciprocal = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_amdgpu_emit_vgpr_binary_immediate(
      context, source_op, LOOM_AMDGPU_DESCRIPTOR_REF_V_MUL_F32_LIT, reciprocal,
      UINT32_C(0x4F7FFFFE), vgpr_type, &scaled_reciprocal));
  loom_value_id_t vector_seed = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_amdgpu_emit_vgpr_unary(
      context, source_op, LOOM_AMDGPU_DESCRIPTOR_REF_V_CVT_U32_F32,
      scaled_reciprocal, vgpr_type, &vector_seed));
  if (register_class_id == LOOM_AMDGPU_REG_CLASS_ID_VGPR) {
    *out_seed = vector_seed;
    return iree_ok_status();
  }

  const loom_value_id_t operands[] = {vector_seed};
  loom_op_t* read_op = NULL;
  IREE_RETURN_IF_ERROR(loom_amdgpu_emit_low_op(
      context, source_op, LOOM_AMDGPU_DESCRIPTOR_REF_V_READFIRSTLANE_B32,
      operands, IREE_ARRAYSIZE(operands), loom_named_attr_slice_empty(),
      &word_type, 1, &read_op));
  *out_seed = loom_value_slice_get(loom_low_op_results(read_op), 0);
  return iree_ok_status();
}

static iree_status_t loom_amdgpu_dynamic_division_emit_subtract_borrow(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    uint16_t register_class_id, loom_value_id_t lhs, loom_value_id_t rhs,
    loom_type_t word_type, loom_value_id_t* out_difference,
    loom_value_id_t* out_borrow) {
  loom_type_t predicate_type = loom_type_none();
  if (register_class_id == LOOM_AMDGPU_REG_CLASS_ID_SGPR) {
    IREE_RETURN_IF_ERROR(loom_amdgpu_make_scc_type(context, &predicate_type));
  } else {
    IREE_RETURN_IF_ERROR(
        loom_amdgpu_make_sgpr_range_type(context, 2, &predicate_type));
  }
  const loom_value_id_t operands[] = {lhs, rhs};
  const loom_type_t result_types[] = {word_type, predicate_type};
  loom_op_t* subtract_op = NULL;
  IREE_RETURN_IF_ERROR(loom_amdgpu_emit_low_op(
      context, source_op,
      register_class_id == LOOM_AMDGPU_REG_CLASS_ID_SGPR
          ? LOOM_AMDGPU_DESCRIPTOR_REF_S_SUB_CO_U32
          : LOOM_AMDGPU_DESCRIPTOR_REF_V_SUB_CO_U32,
      operands, IREE_ARRAYSIZE(operands), loom_named_attr_slice_empty(),
      result_types, IREE_ARRAYSIZE(result_types), &subtract_op));
  *out_difference = loom_value_slice_get(loom_low_op_results(subtract_op), 0);
  *out_borrow = loom_value_slice_get(loom_low_op_results(subtract_op), 1);
  return iree_ok_status();
}

static iree_status_t loom_amdgpu_dynamic_division_emit_select(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    uint16_t register_class_id, loom_value_id_t false_value,
    loom_value_id_t true_value, loom_value_id_t condition,
    loom_type_t word_type, loom_value_id_t* out_value) {
  if (register_class_id == LOOM_AMDGPU_REG_CLASS_ID_VGPR) {
    return loom_amdgpu_emit_vgpr_select(context, source_op, false_value,
                                        true_value, condition, word_type,
                                        out_value);
  }
  const loom_value_id_t operands[] = {true_value, false_value, condition};
  loom_op_t* select_op = NULL;
  IREE_RETURN_IF_ERROR(loom_amdgpu_emit_low_op(
      context, source_op, LOOM_AMDGPU_DESCRIPTOR_REF_S_CSELECT_B32, operands,
      IREE_ARRAYSIZE(operands), loom_named_attr_slice_empty(), &word_type, 1,
      &select_op));
  *out_value = loom_value_slice_get(loom_low_op_results(select_op), 0);
  return iree_ok_status();
}

static iree_status_t loom_amdgpu_dynamic_division_correct(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    uint16_t register_class_id, loom_value_id_t divisor, loom_value_id_t one,
    loom_type_t word_type, loom_value_id_t* inout_quotient,
    loom_value_id_t* inout_remainder) {
  loom_value_id_t incremented = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_amdgpu_dynamic_division_emit_binary(
      context, source_op, register_class_id,
      LOOM_AMDGPU_DESCRIPTOR_REF_S_ADD_U32,
      LOOM_AMDGPU_DESCRIPTOR_REF_V_ADD_U32, *inout_quotient, one, word_type,
      &incremented));
  loom_value_id_t difference = LOOM_VALUE_ID_INVALID;
  loom_value_id_t borrow = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_amdgpu_dynamic_division_emit_subtract_borrow(
      context, source_op, register_class_id, *inout_remainder, divisor,
      word_type, &difference, &borrow));
  loom_value_id_t quotient = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_amdgpu_dynamic_division_emit_select(
      context, source_op, register_class_id, incremented, *inout_quotient,
      borrow, word_type, &quotient));
  loom_value_id_t remainder = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_amdgpu_dynamic_division_emit_select(
      context, source_op, register_class_id, difference, *inout_remainder,
      borrow, word_type, &remainder));
  *inout_quotient = quotient;
  *inout_remainder = remainder;
  return iree_ok_status();
}

static iree_status_t loom_amdgpu_dynamic_division_emit_unsigned(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    uint16_t register_class_id, loom_value_id_t dividend,
    loom_value_id_t divisor, loom_type_t word_type,
    loom_value_id_t* out_quotient, loom_value_id_t* out_remainder) {
  loom_value_id_t zero = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_amdgpu_dynamic_division_emit_constant(
      context, source_op, register_class_id, 0, word_type, &zero));
  loom_value_id_t one = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_amdgpu_dynamic_division_emit_constant(
      context, source_op, register_class_id, 1, word_type, &one));

  loom_value_id_t reciprocal = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_amdgpu_dynamic_division_emit_reciprocal_seed(
      context, source_op, register_class_id, divisor, word_type, &reciprocal));
  loom_value_id_t divisor_product = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_amdgpu_dynamic_division_emit_binary(
      context, source_op, register_class_id,
      LOOM_AMDGPU_DESCRIPTOR_REF_S_MUL_I32,
      LOOM_AMDGPU_DESCRIPTOR_REF_V_MUL_LO_U32, divisor, reciprocal, word_type,
      &divisor_product));
  loom_value_id_t negative_product = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_amdgpu_dynamic_division_emit_binary(
      context, source_op, register_class_id,
      LOOM_AMDGPU_DESCRIPTOR_REF_S_SUB_U32,
      LOOM_AMDGPU_DESCRIPTOR_REF_V_SUB_U32, zero, divisor_product, word_type,
      &negative_product));
  loom_value_id_t refinement = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_amdgpu_dynamic_division_emit_binary(
      context, source_op, register_class_id,
      LOOM_AMDGPU_DESCRIPTOR_REF_S_MUL_HI_U32,
      LOOM_AMDGPU_DESCRIPTOR_REF_V_MUL_HI_U32, reciprocal, negative_product,
      word_type, &refinement));
  IREE_RETURN_IF_ERROR(loom_amdgpu_dynamic_division_emit_binary(
      context, source_op, register_class_id,
      LOOM_AMDGPU_DESCRIPTOR_REF_S_ADD_U32,
      LOOM_AMDGPU_DESCRIPTOR_REF_V_ADD_U32, reciprocal, refinement, word_type,
      &reciprocal));

  loom_value_id_t quotient = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_amdgpu_dynamic_division_emit_binary(
      context, source_op, register_class_id,
      LOOM_AMDGPU_DESCRIPTOR_REF_S_MUL_HI_U32,
      LOOM_AMDGPU_DESCRIPTOR_REF_V_MUL_HI_U32, dividend, reciprocal, word_type,
      &quotient));
  loom_value_id_t quotient_product = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_amdgpu_dynamic_division_emit_binary(
      context, source_op, register_class_id,
      LOOM_AMDGPU_DESCRIPTOR_REF_S_MUL_I32,
      LOOM_AMDGPU_DESCRIPTOR_REF_V_MUL_LO_U32, quotient, divisor, word_type,
      &quotient_product));
  loom_value_id_t remainder = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_amdgpu_dynamic_division_emit_binary(
      context, source_op, register_class_id,
      LOOM_AMDGPU_DESCRIPTOR_REF_S_SUB_U32,
      LOOM_AMDGPU_DESCRIPTOR_REF_V_SUB_U32, dividend, quotient_product,
      word_type, &remainder));
  for (uint32_t i = 0; i < 2; ++i) {
    IREE_RETURN_IF_ERROR(loom_amdgpu_dynamic_division_correct(
        context, source_op, register_class_id, divisor, one, word_type,
        &quotient, &remainder));
  }
  *out_quotient = quotient;
  *out_remainder = remainder;
  return iree_ok_status();
}

iree_status_t loom_amdgpu_lower_dynamic_u32_division(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    const loom_amdgpu_dynamic_u32_division_plan_t* plan) {
  loom_value_id_t dividend = loom_low_lower_lookup_value(context, plan->lhs);
  loom_value_id_t divisor = loom_low_lower_lookup_value(context, plan->rhs);
  const bool is_index =
      loom_type_element_type(loom_module_value_type(
          loom_low_lower_context_module(context),
          loom_op_const_results(source_op)[0])) == LOOM_SCALAR_TYPE_INDEX;
  if (is_index) {
    const loom_type_t required_type = loom_low_register_type(
        loom_low_lower_context_descriptor_set(context)->stable_id,
        plan->register_class_id, 1);
    IREE_RETURN_IF_ERROR(loom_amdgpu_materialize_low_scalar_carrier(
        context, source_op, dividend, required_type, &dividend));
    IREE_RETURN_IF_ERROR(loom_amdgpu_materialize_low_scalar_carrier(
        context, source_op, divisor, required_type, &divisor));
  } else if (plan->register_class_id == LOOM_AMDGPU_REG_CLASS_ID_VGPR) {
    IREE_RETURN_IF_ERROR(loom_amdgpu_materialize_low_vgpr_b32(
        context, source_op, dividend, &dividend));
    IREE_RETURN_IF_ERROR(loom_amdgpu_materialize_low_vgpr_b32(
        context, source_op, divisor, &divisor));
  }
  const loom_type_t word_type =
      loom_module_value_type(loom_low_lower_context_module(context), dividend);
  const bool is_signed = loom_amdgpu_integer_division_domain(source_op) ==
                         LOOM_AMDGPU_INTEGER_DIVISION_DOMAIN_SIGNED;

  loom_value_id_t dividend_sign = LOOM_VALUE_ID_INVALID;
  loom_value_id_t divisor_sign = LOOM_VALUE_ID_INVALID;
  if (is_signed) {
    IREE_RETURN_IF_ERROR(loom_amdgpu_dynamic_division_emit_sign(
        context, source_op, plan->register_class_id, dividend, word_type,
        &dividend_sign));
    IREE_RETURN_IF_ERROR(loom_amdgpu_dynamic_division_emit_sign(
        context, source_op, plan->register_class_id, divisor, word_type,
        &divisor_sign));
    IREE_RETURN_IF_ERROR(loom_amdgpu_dynamic_division_emit_signed_magnitude(
        context, source_op, plan->register_class_id, dividend, dividend_sign,
        word_type, &dividend));
    IREE_RETURN_IF_ERROR(loom_amdgpu_dynamic_division_emit_signed_magnitude(
        context, source_op, plan->register_class_id, divisor, divisor_sign,
        word_type, &divisor));
  }

  loom_value_id_t quotient = LOOM_VALUE_ID_INVALID;
  loom_value_id_t remainder = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_amdgpu_dynamic_division_emit_unsigned(
      context, source_op, plan->register_class_id, dividend, divisor, word_type,
      &quotient, &remainder));
  if (is_signed) {
    loom_value_id_t quotient_sign = LOOM_VALUE_ID_INVALID;
    IREE_RETURN_IF_ERROR(loom_amdgpu_dynamic_division_emit_binary(
        context, source_op, plan->register_class_id,
        LOOM_AMDGPU_DESCRIPTOR_REF_S_XOR_B32,
        LOOM_AMDGPU_DESCRIPTOR_REF_V_XOR_B32, dividend_sign, divisor_sign,
        word_type, &quotient_sign));
    IREE_RETURN_IF_ERROR(loom_amdgpu_dynamic_division_emit_signed_magnitude(
        context, source_op, plan->register_class_id, quotient, quotient_sign,
        word_type, &quotient));
    IREE_RETURN_IF_ERROR(loom_amdgpu_dynamic_division_emit_signed_magnitude(
        context, source_op, plan->register_class_id, remainder, dividend_sign,
        word_type, &remainder));
  }

  if (plan->quotient != LOOM_VALUE_ID_INVALID) {
    IREE_RETURN_IF_ERROR(
        loom_low_lower_bind_value(context, plan->quotient, quotient));
  }
  if (plan->remainder != LOOM_VALUE_ID_INVALID) {
    IREE_RETURN_IF_ERROR(
        loom_low_lower_bind_value(context, plan->remainder, remainder));
  }
  return iree_ok_status();
}
