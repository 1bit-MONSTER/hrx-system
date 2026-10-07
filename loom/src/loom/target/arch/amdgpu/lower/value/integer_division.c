// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/target/arch/amdgpu/lower/value/integer_division.h"

#include "loom/ir/context.h"
#include "loom/ops/scalar/ops.h"
#include "loom/target/arch/amdgpu/lower/descriptor_ref.h"
#include "loom/target/arch/amdgpu/lower/emit.h"
#include "loom/target/arch/amdgpu/lower/legality.h"
#include "loom/target/arch/amdgpu/lower/types.h"
#include "loom/target/arch/amdgpu/lower/value/integer64.h"
#include "loom/util/fact_table.h"

static bool loom_amdgpu_unsigned_i64_divisor(
    const loom_value_fact_table_t* facts, loom_value_id_t rhs,
    uint64_t* out_divisor) {
  int64_t value = 0;
  if (!facts || !loom_value_facts_as_exact_i64(
                    loom_value_fact_table_lookup(facts, rhs), &value)) {
    return false;
  }
  *out_divisor = (uint64_t)value;
  return value != 0;
}

static bool loom_amdgpu_unsigned_i64_division_descriptors_supported(
    const loom_low_descriptor_set_t* descriptors, uint16_t register_class_id) {
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
  return register_class_id == LOOM_AMDGPU_REG_CLASS_ID_SGPR
             ? loom_amdgpu_descriptor_set_has_all_refs(descriptors, sgpr,
                                                       IREE_ARRAYSIZE(sgpr))
             : loom_amdgpu_descriptor_set_has_all_refs(descriptors, vgpr,
                                                       IREE_ARRAYSIZE(vgpr));
}

iree_status_t loom_amdgpu_select_unsigned_i64_division_plan(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    loom_amdgpu_unsigned_i64_division_plan_t* out_plan, bool* out_selected) {
  *out_plan = (loom_amdgpu_unsigned_i64_division_plan_t){0};
  *out_selected = false;
  const loom_module_t* module = loom_low_lower_context_module(context);
  const loom_value_id_t result = loom_op_const_results(source_op)[0];
  if (!loom_amdgpu_type_is_i64(loom_module_value_type(module, result))) {
    return iree_ok_status();
  }
  const loom_value_id_t* operands = loom_op_const_operands(source_op);
  uint64_t divisor = 0;
  if (!loom_amdgpu_unsigned_i64_divisor(
          loom_low_lower_context_fact_table(context), operands[1], &divisor)) {
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
  if (!loom_amdgpu_unsigned_i64_division_descriptors_supported(
          loom_low_lower_context_descriptor_set(context), register_class_id)) {
    return iree_ok_status();
  }
  *out_plan = (loom_amdgpu_unsigned_i64_division_plan_t){
      .source = operands[0],
      .result = result,
      .divisor = divisor,
      .register_class_id = register_class_id,
  };
  if (divisor > 1) {
    out_plan->magic = loom_low_lower_unsigned_divisor_magic_info(divisor, 64);
  }
  *out_selected = true;
  return iree_ok_status();
}

iree_status_t loom_amdgpu_low_legality_verify_unsigned_i64_division(
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
  uint64_t divisor = 0;
  if (!loom_amdgpu_unsigned_i64_divisor(
          loom_target_low_legality_fact_table(context),
          loom_op_const_operands(op)[1], &divisor)) {
    return loom_amdgpu_low_legality_reject(
        context, op, IREE_SV("unsigned_i64_division.constant_nonzero_divisor"));
  }
  bool prefers_vgpr = false;
  IREE_RETURN_IF_ERROR(loom_amdgpu_target_low_legality_value_prefers_vgpr(
      context, result, &prefers_vgpr));
  if (!loom_amdgpu_unsigned_i64_division_descriptors_supported(
          loom_target_low_legality_descriptor_set(context),
          prefers_vgpr ? LOOM_AMDGPU_REG_CLASS_ID_VGPR
                       : LOOM_AMDGPU_REG_CLASS_ID_SGPR)) {
    return loom_amdgpu_low_legality_reject(
        context, op, IREE_SV("unsigned_i64_division.word_arithmetic"));
  }
  return iree_ok_status();
}

static iree_status_t loom_amdgpu_division_emit_constant(
    loom_low_lower_context_t* context, const loom_op_t* op, uint64_t value,
    loom_type_t pair_type, loom_value_id_t* out_value) {
  const loom_type_t word_type =
      loom_low_register_carrier_type_with_unit_count(pair_type, 1);
  const loom_amdgpu_descriptor_ref_t move =
      loom_low_register_type_class_id(pair_type) ==
              LOOM_AMDGPU_REG_CLASS_ID_SGPR
          ? LOOM_AMDGPU_DESCRIPTOR_REF_S_MOV_B32
          : LOOM_AMDGPU_DESCRIPTOR_REF_V_MOV_B32;
  loom_value_id_t words[2];
  IREE_RETURN_IF_ERROR(loom_amdgpu_emit_const_u32(
      context, op, move, (uint32_t)value, word_type, &words[0]));
  IREE_RETURN_IF_ERROR(loom_amdgpu_emit_const_u32(
      context, op, move, (uint32_t)(value >> 32), word_type, &words[1]));
  return loom_amdgpu_build_low_register_range(
      context, op, words, IREE_ARRAYSIZE(words), pair_type, out_value);
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

static iree_status_t loom_amdgpu_division_emit_shift(
    loom_low_lower_context_t* context, const loom_op_t* op,
    loom_value_id_t value, uint8_t shift, loom_value_id_t* out_value) {
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
      uniform ? LOOM_AMDGPU_DESCRIPTOR_REF_S_LSHR_B64
              : LOOM_AMDGPU_DESCRIPTOR_REF_V_LSHRREV_B64,
      uniform ? value : amount, uniform ? amount : value, type, out_value);
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

iree_status_t loom_amdgpu_lower_unsigned_i64_division(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    const loom_amdgpu_unsigned_i64_division_plan_t* plan) {
  loom_value_id_t numerator;
  IREE_RETURN_IF_ERROR(
      loom_low_lower_lookup_value(context, plan->source, &numerator));
  if (plan->register_class_id == LOOM_AMDGPU_REG_CLASS_ID_VGPR) {
    IREE_RETURN_IF_ERROR(loom_amdgpu_materialize_low_vgpr_b32_registers(
        context, source_op, numerator, &numerator));
  }
  const loom_type_t pair_type =
      loom_module_value_type(loom_low_lower_context_module(context), numerator);
  const bool remainder = loom_scalar_remui_isa(source_op);
  loom_value_id_t quotient = numerator;
  if (plan->divisor == 1) {
    if (remainder) {
      IREE_RETURN_IF_ERROR(loom_amdgpu_division_emit_constant(
          context, source_op, 0, pair_type, &quotient));
    }
    return loom_low_lower_bind_value(context, plan->result, quotient);
  }
  loom_value_id_t multiplier;
  IREE_RETURN_IF_ERROR(loom_amdgpu_division_emit_constant(
      context, source_op, plan->magic.multiplier, pair_type, &multiplier));
  IREE_RETURN_IF_ERROR(loom_amdgpu_division_emit_multiply_high(
      context, source_op, numerator, multiplier, &quotient));
  if (plan->magic.is_add) {
    loom_value_id_t difference;
    IREE_RETURN_IF_ERROR(loom_amdgpu_division_emit_subtract(
        context, source_op, numerator, quotient, &difference));
    IREE_RETURN_IF_ERROR(loom_amdgpu_division_emit_shift(
        context, source_op, difference, 1, &difference));
    IREE_RETURN_IF_ERROR(loom_amdgpu_division_emit_add(
        context, source_op, difference, quotient, &quotient));
  }
  IREE_RETURN_IF_ERROR(loom_amdgpu_division_emit_shift(
      context, source_op, quotient, plan->magic.post_shift, &quotient));
  if (remainder) {
    loom_value_id_t divisor;
    IREE_RETURN_IF_ERROR(loom_amdgpu_division_emit_constant(
        context, source_op, plan->divisor, pair_type, &divisor));
    loom_value_id_t product;
    IREE_RETURN_IF_ERROR(loom_amdgpu_emit_i64_mul_lo(
        context, source_op, quotient, divisor, &product));
    IREE_RETURN_IF_ERROR(loom_amdgpu_division_emit_subtract(
        context, source_op, numerator, product, &quotient));
  }
  return loom_low_lower_bind_value(context, plan->result, quotient);
}
