// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/target/arch/amdgpu/lower/value/integer_division.h"

#include "loom/ir/context.h"
#include "loom/ops/index/ops.h"
#include "loom/ops/scalar/ops.h"
#include "loom/target/arch/amdgpu/lower/descriptor_ref.h"
#include "loom/target/arch/amdgpu/lower/emit.h"
#include "loom/target/arch/amdgpu/lower/legality.h"
#include "loom/target/arch/amdgpu/lower/types.h"
#include "loom/target/arch/amdgpu/lower/value/integer64.h"
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

static bool loom_amdgpu_i64_divisor(const loom_value_fact_table_t* facts,
                                    loom_value_id_t rhs,
                                    uint64_t* out_divisor) {
  int64_t value = 0;
  if (!loom_amdgpu_exact_i64(facts, rhs, &value) || value == 0) {
    return false;
  }
  *out_divisor = (uint64_t)value;
  return true;
}

static bool loom_amdgpu_i64_division_descriptors_supported(
    const loom_low_descriptor_set_t* descriptors, uint16_t register_class_id,
    loom_amdgpu_integer_division_domain_t domain) {
  static const loom_amdgpu_descriptor_ref_t sgpr[] = {
      LOOM_AMDGPU_DESCRIPTOR_REF_S_MOV_B32,
      LOOM_AMDGPU_DESCRIPTOR_REF_S_MUL_I32,
      LOOM_AMDGPU_DESCRIPTOR_REF_S_MUL_HI_U32,
      LOOM_AMDGPU_DESCRIPTOR_REF_S_ADD_U32,
      LOOM_AMDGPU_DESCRIPTOR_REF_S_ADD_CO_U32,
      LOOM_AMDGPU_DESCRIPTOR_REF_S_ADDC_U32,
      LOOM_AMDGPU_DESCRIPTOR_REF_S_SUB_CO_U32,
      LOOM_AMDGPU_DESCRIPTOR_REF_S_SUBB_U32,
      LOOM_AMDGPU_DESCRIPTOR_REF_S_LSHR_B64,
  };
  static const loom_amdgpu_descriptor_ref_t vgpr[] = {
      LOOM_AMDGPU_DESCRIPTOR_REF_V_MOV_B32,
      LOOM_AMDGPU_DESCRIPTOR_REF_V_MOV_B32_COPY,
      LOOM_AMDGPU_DESCRIPTOR_REF_V_MUL_LO_U32,
      LOOM_AMDGPU_DESCRIPTOR_REF_V_MUL_HI_U32,
      LOOM_AMDGPU_DESCRIPTOR_REF_V_ADD_U32,
      LOOM_AMDGPU_DESCRIPTOR_REF_V_ADD_CO_U32,
      LOOM_AMDGPU_DESCRIPTOR_REF_V_ADD_CO_CI_U32,
      LOOM_AMDGPU_DESCRIPTOR_REF_V_SUB_CO_U32,
      LOOM_AMDGPU_DESCRIPTOR_REF_V_SUB_CO_CI_U32,
      LOOM_AMDGPU_DESCRIPTOR_REF_V_LSHRREV_B64,
  };
  static const loom_amdgpu_descriptor_ref_t signed_sgpr[] = {
      LOOM_AMDGPU_DESCRIPTOR_REF_S_AND_B64,
      LOOM_AMDGPU_DESCRIPTOR_REF_S_ASHR_I64,
  };
  static const loom_amdgpu_descriptor_ref_t signed_vgpr[] = {
      LOOM_AMDGPU_DESCRIPTOR_REF_V_AND_B32,
      LOOM_AMDGPU_DESCRIPTOR_REF_V_ASHRREV_I64,
  };
  const bool uniform = register_class_id == LOOM_AMDGPU_REG_CLASS_ID_SGPR;
  if (!loom_amdgpu_descriptor_set_has_all_refs(
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

static loom_amdgpu_integer_division_plan_t
loom_amdgpu_integer_division_plan_empty(void) {
  return (loom_amdgpu_integer_division_plan_t){
      .lhs = LOOM_VALUE_ID_INVALID,
      .rhs = LOOM_VALUE_ID_INVALID,
  };
}

static void loom_amdgpu_integer_division_results(
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
      IREE_ASSERT_UNREACHABLE("unexpected integer division operation");
      IREE_BUILTIN_UNREACHABLE();
  }
}

static iree_status_t loom_amdgpu_select_i64_division_plan(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    loom_amdgpu_integer_division_plan_t* out_plan, bool* out_selected) {
  const loom_module_t* module = loom_low_lower_context_module(context);
  const loom_value_id_t result = loom_op_const_results(source_op)[0];
  if (!loom_amdgpu_type_is_i64(loom_module_value_type(module, result))) {
    return iree_ok_status();
  }
  const loom_value_id_t* operands = loom_op_const_operands(source_op);
  const loom_amdgpu_integer_division_domain_t domain =
      loom_amdgpu_integer_division_domain(source_op);
  uint64_t divisor = 0;
  if (!loom_amdgpu_i64_divisor(loom_low_lower_context_fact_table(context),
                               operands[1], &divisor)) {
    return iree_ok_status();
  }

  loom_type_t result_type = loom_type_none();
  IREE_RETURN_IF_ERROR(
      loom_amdgpu_low_result_type(context, source_op, result, &result_type));
  if (!loom_amdgpu_low_type_is_register_class_count(
          context, result_type, LOOM_AMDGPU_REG_CLASS_ID_SGPR, 2) &&
      !loom_amdgpu_low_type_is_register_class_count(
          context, result_type, LOOM_AMDGPU_REG_CLASS_ID_VGPR, 2)) {
    return iree_ok_status();
  }
  const uint16_t register_class_id =
      loom_low_register_type_class_id(result_type);
  loom_type_t source_type = loom_type_none();
  IREE_RETURN_IF_ERROR(
      loom_low_lower_map_value(context, source_op, operands[0], &source_type));
  if (!loom_amdgpu_low_type_is_register_class_count(context, source_type,
                                                    register_class_id, 2) &&
      !(register_class_id == LOOM_AMDGPU_REG_CLASS_ID_VGPR &&
        loom_amdgpu_low_type_is_register_class_count(
            context, source_type, LOOM_AMDGPU_REG_CLASS_ID_SGPR, 2))) {
    return iree_ok_status();
  }
  if (!loom_amdgpu_i64_division_descriptors_supported(
          loom_low_lower_context_descriptor_set(context), register_class_id,
          domain)) {
    return iree_ok_status();
  }
  *out_plan = (loom_amdgpu_integer_division_plan_t){
      .lhs = operands[0],
      .rhs = LOOM_VALUE_ID_INVALID,
      .divisor = divisor,
      .register_class_id = register_class_id,
      .kind = LOOM_AMDGPU_INTEGER_DIVISION_PLAN_CONSTANT_I64,
  };
  if (domain == LOOM_AMDGPU_INTEGER_DIVISION_DOMAIN_SIGNED) {
    const int64_t signed_divisor = (int64_t)divisor;
    if (signed_divisor != 1 && signed_divisor != -1) {
      out_plan->magic.signed_value =
          loom_low_lower_signed_divisor_magic_info(signed_divisor, 64);
    }
  } else {
    const uint64_t numerator_maximum =
        loom_low_lower_unsigned_numerator_maximum(
            loom_low_lower_context_fact_table(context), operands[0], 64);
    out_plan->quotient_unit_count =
        numerator_maximum / divisor <= UINT32_MAX ? 1 : 2;
    if (divisor > 1) {
      out_plan->magic.unsigned_value =
          loom_low_lower_unsigned_divisor_magic_info(divisor, 64,
                                                     numerator_maximum);
    }
  }
  *out_selected = true;
  return iree_ok_status();
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

static iree_status_t loom_amdgpu_select_dynamic_u32_division_plan(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    loom_amdgpu_integer_division_plan_t* out_plan, bool* out_selected) {
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
  loom_amdgpu_integer_division_results(source_op, &quotient, &remainder);
  if (loom_type_element_type(loom_module_value_type(
          module, loom_op_const_results(source_op)[0])) ==
          LOOM_SCALAR_TYPE_INDEX &&
      !loom_amdgpu_dynamic_index_division_facts_supported(
          loom_low_lower_context_fact_table(context), operands[0], operands[1],
          quotient, remainder)) {
    return iree_ok_status();
  }
  const loom_value_id_t values[] = {operands[0], operands[1], quotient,
                                    remainder};
  uint16_t register_class_id = LOOM_AMDGPU_REG_CLASS_ID_SGPR;
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
  const loom_amdgpu_integer_division_domain_t domain =
      loom_amdgpu_integer_division_domain(source_op);
  if (!loom_amdgpu_u32_division_descriptors_supported(
          loom_low_lower_context_descriptor_set(context), register_class_id,
          domain)) {
    return iree_ok_status();
  }

  *out_plan = (loom_amdgpu_integer_division_plan_t){
      .lhs = operands[0],
      .rhs = operands[1],
      .register_class_id = register_class_id,
      .kind = LOOM_AMDGPU_INTEGER_DIVISION_PLAN_DYNAMIC_U32,
  };
  *out_selected = true;
  return iree_ok_status();
}

iree_status_t loom_amdgpu_select_integer_division_plan(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    loom_amdgpu_integer_division_plan_t* out_plan, bool* out_selected) {
  *out_plan = loom_amdgpu_integer_division_plan_empty();
  *out_selected = false;
  IREE_RETURN_IF_ERROR(loom_amdgpu_select_i64_division_plan(
      context, source_op, out_plan, out_selected));
  if (*out_selected) {
    return iree_ok_status();
  }
  return loom_amdgpu_select_dynamic_u32_division_plan(context, source_op,
                                                      out_plan, out_selected);
}

iree_status_t loom_amdgpu_finalize_integer_division_plan(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    const loom_amdgpu_integer_division_plan_t* plan) {
  loom_value_id_t quotient = LOOM_VALUE_ID_INVALID;
  loom_value_id_t remainder = LOOM_VALUE_ID_INVALID;
  loom_amdgpu_integer_division_results(source_op, &quotient, &remainder);
  loom_type_t result_type =
      loom_low_lower_value_binding_type(context, plan->lhs);
  if (plan->register_class_id == LOOM_AMDGPU_REG_CLASS_ID_VGPR) {
    result_type = loom_low_register_type(
        loom_low_lower_context_descriptor_set(context)->stable_id,
        LOOM_AMDGPU_REG_CLASS_ID_VGPR,
        plan->kind == LOOM_AMDGPU_INTEGER_DIVISION_PLAN_DYNAMIC_U32
            ? 1
            : loom_low_register_type_unit_count(result_type));
  }
  if (quotient != LOOM_VALUE_ID_INVALID) {
    IREE_RETURN_IF_ERROR(
        loom_low_lower_plan_value_type(context, quotient, result_type));
  }
  if (remainder != LOOM_VALUE_ID_INVALID) {
    IREE_RETURN_IF_ERROR(
        loom_low_lower_plan_value_type(context, remainder, result_type));
  }
  return iree_ok_status();
}

void loom_amdgpu_mark_integer_division_plan_storage_demands(
    loom_low_lower_context_t* context,
    const loom_amdgpu_integer_division_plan_t* plan) {
  loom_low_lower_require_source_value_storage(context, plan->lhs);
  if (plan->rhs != LOOM_VALUE_ID_INVALID) {
    loom_low_lower_require_source_value_storage(context, plan->rhs);
  }
}

iree_status_t loom_amdgpu_low_legality_verify_i64_division(
    const loom_target_low_legality_provider_t* provider,
    loom_target_low_legality_context_t* context, const loom_op_t* op,
    bool* out_handled) {
  (void)provider;
  if (!loom_amdgpu_low_legality_context_is_amdgpu(context)) {
    return iree_ok_status();
  }
  const loom_value_id_t result = loom_op_const_results(op)[0];
  if (!loom_amdgpu_type_is_i64(loom_module_value_type(
          loom_target_low_legality_module(context), result))) {
    return iree_ok_status();
  }
  *out_handled = true;
  const loom_amdgpu_integer_division_domain_t domain =
      loom_amdgpu_integer_division_domain(op);
  uint64_t divisor = 0;
  if (!loom_amdgpu_i64_divisor(loom_target_low_legality_fact_table(context),
                               loom_op_const_operands(op)[1], &divisor)) {
    return loom_amdgpu_low_legality_reject(
        context, op,
        domain == LOOM_AMDGPU_INTEGER_DIVISION_DOMAIN_SIGNED
            ? IREE_SV("signed_i64_division.constant_nonzero_divisor")
            : IREE_SV("unsigned_i64_division.constant_nonzero_divisor"));
  }
  bool prefers_vgpr = false;
  IREE_RETURN_IF_ERROR(loom_amdgpu_target_low_legality_value_prefers_vgpr(
      context, result, &prefers_vgpr));
  if (!loom_amdgpu_i64_division_descriptors_supported(
          loom_target_low_legality_descriptor_set(context),
          prefers_vgpr ? LOOM_AMDGPU_REG_CLASS_ID_VGPR
                       : LOOM_AMDGPU_REG_CLASS_ID_SGPR,
          domain)) {
    return loom_amdgpu_low_legality_reject(
        context, op,
        domain == LOOM_AMDGPU_INTEGER_DIVISION_DOMAIN_SIGNED
            ? IREE_SV("signed_i64_division.word_arithmetic")
            : IREE_SV("unsigned_i64_division.word_arithmetic"));
  }
  return iree_ok_status();
}

static iree_status_t loom_amdgpu_division_emit_constant(
    loom_low_lower_context_t* context, const loom_op_t* op, uint64_t value,
    loom_type_t result_type, loom_value_id_t* out_value) {
  const loom_type_t word_type =
      loom_low_register_carrier_type_with_unit_count(result_type, 1);
  const loom_amdgpu_descriptor_ref_t move =
      loom_low_register_type_class_id(result_type) ==
              LOOM_AMDGPU_REG_CLASS_ID_SGPR
          ? LOOM_AMDGPU_DESCRIPTOR_REF_S_MOV_B32
          : LOOM_AMDGPU_DESCRIPTOR_REF_V_MOV_B32;
  loom_value_id_t words[2];
  IREE_RETURN_IF_ERROR(loom_amdgpu_emit_const_u32(
      context, op, move, (uint32_t)value, word_type, &words[0]));
  if (loom_low_register_type_unit_count(result_type) == 1) {
    *out_value = words[0];
    return iree_ok_status();
  }
  IREE_RETURN_IF_ERROR(loom_amdgpu_emit_const_u32(
      context, op, move, (uint32_t)(value >> 32), word_type, &words[1]));
  return loom_amdgpu_build_low_register_range(
      context, op, words, IREE_ARRAYSIZE(words), result_type, out_value);
}

static iree_status_t loom_amdgpu_division_emit_add(
    loom_low_lower_context_t* context, const loom_op_t* op, loom_value_id_t lhs,
    loom_value_id_t rhs, loom_value_id_t* out_value) {
  const loom_type_t type =
      loom_module_value_type(loom_low_lower_context_module(context), lhs);
  if (loom_low_register_type_class_id(type) == LOOM_AMDGPU_REG_CLASS_ID_SGPR) {
    return loom_amdgpu_emit_sgpr64_binary_carry(
        context, op, LOOM_AMDGPU_DESCRIPTOR_REF_S_ADD_CO_U32,
        LOOM_AMDGPU_DESCRIPTOR_REF_S_ADDC_U32, lhs, rhs, out_value);
  }
  return loom_amdgpu_emit_vgpr64_add(context, op, lhs, rhs, out_value);
}

static iree_status_t loom_amdgpu_division_emit_subtract(
    loom_low_lower_context_t* context, const loom_op_t* op, loom_value_id_t lhs,
    loom_value_id_t rhs, loom_value_id_t* out_value) {
  const loom_type_t type =
      loom_module_value_type(loom_low_lower_context_module(context), lhs);
  if (loom_low_register_type_class_id(type) == LOOM_AMDGPU_REG_CLASS_ID_SGPR) {
    return loom_amdgpu_emit_sgpr64_binary_carry(
        context, op, LOOM_AMDGPU_DESCRIPTOR_REF_S_SUB_CO_U32,
        LOOM_AMDGPU_DESCRIPTOR_REF_S_SUBB_U32, lhs, rhs, out_value);
  }
  return loom_amdgpu_emit_vgpr64_sub(context, op, lhs, rhs, out_value);
}

typedef enum loom_amdgpu_division_shift_kind_e {
  LOOM_AMDGPU_DIVISION_SHIFT_LOGICAL_RIGHT = 0,
  LOOM_AMDGPU_DIVISION_SHIFT_ARITHMETIC_RIGHT = 1,
} loom_amdgpu_division_shift_kind_t;

static iree_status_t loom_amdgpu_division_emit_shift(
    loom_low_lower_context_t* context, const loom_op_t* op,
    loom_value_id_t value, uint8_t shift,
    loom_amdgpu_division_shift_kind_t kind, loom_value_id_t* out_value) {
  if (shift == 0) {
    *out_value = value;
    return iree_ok_status();
  }
  const loom_type_t type =
      loom_module_value_type(loom_low_lower_context_module(context), value);
  const loom_type_t word_type =
      loom_low_register_carrier_type_with_unit_count(type, 1);
  const bool uniform =
      loom_low_register_type_class_id(type) == LOOM_AMDGPU_REG_CLASS_ID_SGPR;
  loom_value_id_t amount;
  IREE_RETURN_IF_ERROR(
      loom_amdgpu_emit_const_u32(context, op,
                                 uniform ? LOOM_AMDGPU_DESCRIPTOR_REF_S_MOV_B32
                                         : LOOM_AMDGPU_DESCRIPTOR_REF_V_MOV_B32,
                                 shift, word_type, &amount));
  return loom_amdgpu_emit_binary(
      context, op,
      uniform ? (kind == LOOM_AMDGPU_DIVISION_SHIFT_ARITHMETIC_RIGHT
                     ? LOOM_AMDGPU_DESCRIPTOR_REF_S_ASHR_I64
                     : LOOM_AMDGPU_DESCRIPTOR_REF_S_LSHR_B64)
              : (kind == LOOM_AMDGPU_DIVISION_SHIFT_ARITHMETIC_RIGHT
                     ? LOOM_AMDGPU_DESCRIPTOR_REF_V_ASHRREV_I64
                     : LOOM_AMDGPU_DESCRIPTOR_REF_V_LSHRREV_B64),
      uniform ? value : amount, uniform ? amount : value, type, out_value);
}

static iree_status_t loom_amdgpu_division_emit_and(
    loom_low_lower_context_t* context, const loom_op_t* op, loom_value_id_t lhs,
    loom_value_id_t rhs, loom_value_id_t* out_value) {
  const loom_type_t pair_type =
      loom_module_value_type(loom_low_lower_context_module(context), lhs);
  if (loom_low_register_type_class_id(pair_type) ==
      LOOM_AMDGPU_REG_CLASS_ID_SGPR) {
    return loom_amdgpu_emit_binary(context, op,
                                   LOOM_AMDGPU_DESCRIPTOR_REF_S_AND_B64, lhs,
                                   rhs, pair_type, out_value);
  }
  const loom_type_t word_type =
      loom_low_register_carrier_type_with_unit_count(pair_type, 1);
  loom_value_id_t words[2];
  for (uint32_t i = 0; i < IREE_ARRAYSIZE(words); ++i) {
    loom_value_id_t lhs_word;
    IREE_RETURN_IF_ERROR(
        loom_amdgpu_emit_low_slice(context, op, lhs, i, word_type, &lhs_word));
    loom_value_id_t rhs_word;
    IREE_RETURN_IF_ERROR(
        loom_amdgpu_emit_low_slice(context, op, rhs, i, word_type, &rhs_word));
    IREE_RETURN_IF_ERROR(loom_amdgpu_emit_binary(
        context, op, LOOM_AMDGPU_DESCRIPTOR_REF_V_AND_B32, lhs_word, rhs_word,
        word_type, &words[i]));
  }
  return loom_amdgpu_build_low_register_range(
      context, op, words, IREE_ARRAYSIZE(words), pair_type, out_value);
}

// At radix B=2^32, form t1=a1*b0+hi(a0*b0), t2=a0*b1+lo(t1),
// then high64(a*b)=a1*b1+hi(t1)+hi(t2). Each temporary is an exact u64:
// a 32x32 product is at most (B-1)^2 and its added word is at most B-1.
// Native carry results stay explicit SSA edges through scheduling/allocation.
static iree_status_t loom_amdgpu_division_emit_multiply_high(
    loom_low_lower_context_t* context, const loom_op_t* op, loom_value_id_t lhs,
    loom_value_id_t rhs, loom_value_id_t* out_value) {
  const loom_type_t pair_type =
      loom_module_value_type(loom_low_lower_context_module(context), lhs);
  const loom_type_t word_type =
      loom_low_register_carrier_type_with_unit_count(pair_type, 1);
  const bool uniform = loom_low_register_type_class_id(pair_type) ==
                       LOOM_AMDGPU_REG_CLASS_ID_SGPR;
  const loom_amdgpu_descriptor_ref_t multiply_low =
      uniform ? LOOM_AMDGPU_DESCRIPTOR_REF_S_MUL_I32
              : LOOM_AMDGPU_DESCRIPTOR_REF_V_MUL_LO_U32;
  const loom_amdgpu_descriptor_ref_t multiply_high =
      uniform ? LOOM_AMDGPU_DESCRIPTOR_REF_S_MUL_HI_U32
              : LOOM_AMDGPU_DESCRIPTOR_REF_V_MUL_HI_U32;
  loom_value_id_t lhs_words[2], rhs_words[2];
  for (uint32_t i = 0; i < 2; ++i) {
    IREE_RETURN_IF_ERROR(loom_amdgpu_emit_low_slice(context, op, lhs, i,
                                                    word_type, &lhs_words[i]));
    IREE_RETURN_IF_ERROR(loom_amdgpu_emit_low_slice(context, op, rhs, i,
                                                    word_type, &rhs_words[i]));
  }
  loom_value_id_t zero;
  IREE_RETURN_IF_ERROR(
      loom_amdgpu_emit_const_u32(context, op,
                                 uniform ? LOOM_AMDGPU_DESCRIPTOR_REF_S_MOV_B32
                                         : LOOM_AMDGPU_DESCRIPTOR_REF_V_MOV_B32,
                                 0, word_type, &zero));
  loom_value_id_t low_product_high;
  IREE_RETURN_IF_ERROR(loom_amdgpu_emit_binary(context, op, multiply_high,
                                               lhs_words[0], rhs_words[0],
                                               word_type, &low_product_high));

  // Accumulate each cross product before publishing its high word. Splitting
  // a pair here never discards the carry from the preceding low-word sum.
  loom_value_id_t cross_high[2];
  loom_value_id_t addend = low_product_high;
  for (uint32_t i = 0; i < 2; ++i) {
    loom_value_id_t product_words[2];
    IREE_RETURN_IF_ERROR(loom_amdgpu_emit_binary(context, op, multiply_low,
                                                 lhs_words[1 - i], rhs_words[i],
                                                 word_type, &product_words[0]));
    IREE_RETURN_IF_ERROR(loom_amdgpu_emit_binary(context, op, multiply_high,
                                                 lhs_words[1 - i], rhs_words[i],
                                                 word_type, &product_words[1]));
    loom_value_id_t product;
    IREE_RETURN_IF_ERROR(loom_amdgpu_build_low_register_range(
        context, op, product_words, 2, pair_type, &product));
    const loom_value_id_t addend_words[] = {addend, zero};
    loom_value_id_t wide_addend;
    IREE_RETURN_IF_ERROR(loom_amdgpu_build_low_register_range(
        context, op, addend_words, 2, pair_type, &wide_addend));
    loom_value_id_t sum;
    IREE_RETURN_IF_ERROR(
        loom_amdgpu_division_emit_add(context, op, product, wide_addend, &sum));
    IREE_RETURN_IF_ERROR(
        loom_amdgpu_emit_low_slice(context, op, sum, 0, word_type, &addend));
    IREE_RETURN_IF_ERROR(loom_amdgpu_emit_low_slice(context, op, sum, 1,
                                                    word_type, &cross_high[i]));
  }
  loom_value_id_t high_product_words[2];
  IREE_RETURN_IF_ERROR(
      loom_amdgpu_emit_binary(context, op, multiply_low, lhs_words[1],
                              rhs_words[1], word_type, &high_product_words[0]));
  IREE_RETURN_IF_ERROR(
      loom_amdgpu_emit_binary(context, op, multiply_high, lhs_words[1],
                              rhs_words[1], word_type, &high_product_words[1]));
  loom_value_id_t result;
  IREE_RETURN_IF_ERROR(loom_amdgpu_build_low_register_range(
      context, op, high_product_words, 2, pair_type, &result));
  for (uint32_t i = 0; i < 2; ++i) {
    const loom_value_id_t words[] = {cross_high[i], zero};
    loom_value_id_t wide_cross_high;
    IREE_RETURN_IF_ERROR(loom_amdgpu_build_low_register_range(
        context, op, words, 2, pair_type, &wide_cross_high));
    IREE_RETURN_IF_ERROR(loom_amdgpu_division_emit_add(
        context, op, result, wide_cross_high, &result));
  }
  *out_value = result;
  return iree_ok_status();
}

static iree_status_t loom_amdgpu_division_emit_unsigned_quotient(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    const loom_amdgpu_integer_division_plan_t* plan, loom_value_id_t numerator,
    loom_type_t pair_type, loom_value_id_t* out_quotient) {
  if (plan->divisor == 1) {
    *out_quotient = numerator;
    return iree_ok_status();
  }
  const loom_low_lower_unsigned_divisor_magic_info_t magic =
      plan->magic.unsigned_value;
  if (magic.multiplier == 0) {
    return loom_amdgpu_division_emit_constant(context, source_op, 0, pair_type,
                                              out_quotient);
  }
  loom_value_id_t multiplier;
  IREE_RETURN_IF_ERROR(loom_amdgpu_division_emit_constant(
      context, source_op, magic.multiplier, pair_type, &multiplier));
  loom_value_id_t quotient;
  IREE_RETURN_IF_ERROR(loom_amdgpu_division_emit_multiply_high(
      context, source_op, numerator, multiplier, &quotient));
  if (magic.is_add) {
    loom_value_id_t difference;
    IREE_RETURN_IF_ERROR(loom_amdgpu_division_emit_subtract(
        context, source_op, numerator, quotient, &difference));
    IREE_RETURN_IF_ERROR(loom_amdgpu_division_emit_shift(
        context, source_op, difference, 1,
        LOOM_AMDGPU_DIVISION_SHIFT_LOGICAL_RIGHT, &difference));
    IREE_RETURN_IF_ERROR(loom_amdgpu_division_emit_add(
        context, source_op, difference, quotient, &quotient));
  }
  return loom_amdgpu_division_emit_shift(
      context, source_op, quotient, magic.post_shift,
      LOOM_AMDGPU_DIVISION_SHIFT_LOGICAL_RIGHT, out_quotient);
}

static iree_status_t loom_amdgpu_division_emit_signed_quotient(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    const loom_amdgpu_integer_division_plan_t* plan, loom_value_id_t numerator,
    loom_type_t pair_type, loom_value_id_t* out_quotient) {
  const int64_t divisor = (int64_t)plan->divisor;
  if (divisor == 1) {
    *out_quotient = numerator;
    return iree_ok_status();
  }
  if (divisor == -1) {
    loom_value_id_t zero;
    IREE_RETURN_IF_ERROR(loom_amdgpu_division_emit_constant(
        context, source_op, 0, pair_type, &zero));
    return loom_amdgpu_division_emit_subtract(context, source_op, zero,
                                              numerator, out_quotient);
  }

  const loom_low_lower_signed_divisor_magic_info_t magic =
      plan->magic.signed_value;
  loom_value_id_t multiplier;
  IREE_RETURN_IF_ERROR(loom_amdgpu_division_emit_constant(
      context, source_op, magic.multiplier, pair_type, &multiplier));
  loom_value_id_t quotient;
  IREE_RETURN_IF_ERROR(loom_amdgpu_division_emit_multiply_high(
      context, source_op, numerator, multiplier, &quotient));

  // Convert the unsigned high product to the adjusted signed high product in
  // one combined recipe. The numerator sign contributes -multiplier when
  // negative. The signed magic numerator coefficient cancels the multiplier
  // sign correction for positive divisors and combines to -numerator for every
  // negative divisor.
  loom_value_id_t numerator_sign;
  IREE_RETURN_IF_ERROR(loom_amdgpu_division_emit_shift(
      context, source_op, numerator, 63,
      LOOM_AMDGPU_DIVISION_SHIFT_ARITHMETIC_RIGHT, &numerator_sign));
  loom_value_id_t signed_high_correction;
  IREE_RETURN_IF_ERROR(loom_amdgpu_division_emit_and(
      context, source_op, numerator_sign, multiplier, &signed_high_correction));
  IREE_RETURN_IF_ERROR(loom_amdgpu_division_emit_subtract(
      context, source_op, quotient, signed_high_correction, &quotient));
  if (divisor < 0) {
    IREE_RETURN_IF_ERROR(loom_amdgpu_division_emit_subtract(
        context, source_op, quotient, numerator, &quotient));
  }
  IREE_RETURN_IF_ERROR(loom_amdgpu_division_emit_shift(
      context, source_op, quotient, magic.post_shift,
      LOOM_AMDGPU_DIVISION_SHIFT_ARITHMETIC_RIGHT, &quotient));
  loom_value_id_t sign_correction;
  IREE_RETURN_IF_ERROR(loom_amdgpu_division_emit_shift(
      context, source_op, quotient, 63,
      LOOM_AMDGPU_DIVISION_SHIFT_LOGICAL_RIGHT, &sign_correction));
  return loom_amdgpu_division_emit_add(context, source_op, quotient,
                                       sign_correction, out_quotient);
}

static iree_status_t loom_amdgpu_lower_constant_i64_division(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    const loom_amdgpu_integer_division_plan_t* plan) {
  loom_value_id_t numerator = loom_low_lower_lookup_value(context, plan->lhs);
  if (plan->register_class_id == LOOM_AMDGPU_REG_CLASS_ID_VGPR) {
    IREE_RETURN_IF_ERROR(loom_amdgpu_materialize_low_vgpr_b32_registers(
        context, source_op, numerator, &numerator));
  }
  const loom_type_t pair_type =
      loom_module_value_type(loom_low_lower_context_module(context), numerator);
  const loom_amdgpu_integer_division_domain_t domain =
      loom_amdgpu_integer_division_domain(source_op);
  loom_value_id_t quotient_result = LOOM_VALUE_ID_INVALID;
  loom_value_id_t remainder_result = LOOM_VALUE_ID_INVALID;
  loom_amdgpu_integer_division_results(source_op, &quotient_result,
                                       &remainder_result);
  const bool produces_remainder = remainder_result != LOOM_VALUE_ID_INVALID;
  const loom_value_id_t result =
      produces_remainder ? remainder_result : quotient_result;
  if (produces_remainder &&
      (plan->divisor == 1 ||
       (domain == LOOM_AMDGPU_INTEGER_DIVISION_DOMAIN_SIGNED &&
        (int64_t)plan->divisor == -1))) {
    loom_value_id_t zero;
    IREE_RETURN_IF_ERROR(loom_amdgpu_division_emit_constant(
        context, source_op, 0, pair_type, &zero));
    return loom_low_lower_bind_value(context, result, zero);
  }
  if (produces_remainder &&
      domain == LOOM_AMDGPU_INTEGER_DIVISION_DOMAIN_UNSIGNED &&
      plan->magic.unsigned_value.multiplier == 0) {
    return loom_low_lower_bind_value(context, result, numerator);
  }

  loom_value_id_t quotient;
  if (domain == LOOM_AMDGPU_INTEGER_DIVISION_DOMAIN_SIGNED) {
    IREE_RETURN_IF_ERROR(loom_amdgpu_division_emit_signed_quotient(
        context, source_op, plan, numerator, pair_type, &quotient));
  } else {
    IREE_RETURN_IF_ERROR(loom_amdgpu_division_emit_unsigned_quotient(
        context, source_op, plan, numerator, pair_type, &quotient));
  }
  if (produces_remainder) {
    const loom_type_t word_type =
        loom_low_register_carrier_type_with_unit_count(pair_type, 1);
    loom_type_t divisor_type = pair_type;
    if (domain == LOOM_AMDGPU_INTEGER_DIVISION_DOMAIN_UNSIGNED &&
        plan->quotient_unit_count == 1) {
      divisor_type = word_type;
      IREE_RETURN_IF_ERROR(loom_amdgpu_emit_low_slice(
          context, source_op, quotient, 0, divisor_type, &quotient));
    }
    loom_value_id_t divisor;
    IREE_RETURN_IF_ERROR(loom_amdgpu_division_emit_constant(
        context, source_op, plan->divisor,
        plan->divisor <= UINT32_MAX ? word_type : pair_type, &divisor));
    loom_value_id_t product;
    IREE_RETURN_IF_ERROR(loom_amdgpu_emit_i64_mul_lo(
        context, source_op, quotient, divisor, &product));
    IREE_RETURN_IF_ERROR(loom_amdgpu_division_emit_subtract(
        context, source_op, numerator, product, &quotient));
  }
  return loom_low_lower_bind_value(context, result, quotient);
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

static iree_status_t loom_amdgpu_lower_dynamic_u32_division(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    const loom_amdgpu_integer_division_plan_t* plan) {
  loom_value_id_t dividend = loom_low_lower_lookup_value(context, plan->lhs);
  loom_value_id_t divisor = loom_low_lower_lookup_value(context, plan->rhs);
  if (plan->register_class_id == LOOM_AMDGPU_REG_CLASS_ID_VGPR) {
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

  loom_value_id_t quotient_result = LOOM_VALUE_ID_INVALID;
  loom_value_id_t remainder_result = LOOM_VALUE_ID_INVALID;
  loom_amdgpu_integer_division_results(source_op, &quotient_result,
                                       &remainder_result);
  if (quotient_result != LOOM_VALUE_ID_INVALID) {
    IREE_RETURN_IF_ERROR(
        loom_low_lower_bind_value(context, quotient_result, quotient));
  }
  if (remainder_result != LOOM_VALUE_ID_INVALID) {
    IREE_RETURN_IF_ERROR(
        loom_low_lower_bind_value(context, remainder_result, remainder));
  }
  return iree_ok_status();
}

iree_status_t loom_amdgpu_lower_integer_division(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    const loom_amdgpu_integer_division_plan_t* plan) {
  switch (plan->kind) {
    case LOOM_AMDGPU_INTEGER_DIVISION_PLAN_CONSTANT_I64:
      return loom_amdgpu_lower_constant_i64_division(context, source_op, plan);
    case LOOM_AMDGPU_INTEGER_DIVISION_PLAN_DYNAMIC_U32:
      return loom_amdgpu_lower_dynamic_u32_division(context, source_op, plan);
    default:
      IREE_ASSERT_UNREACHABLE("unknown AMDGPU integer division plan kind");
      IREE_BUILTIN_UNREACHABLE();
  }
}
