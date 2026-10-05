// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/target/arch/amdgpu/lower/encoding/float64_narrow.h"

#include <stdint.h>

#include "loom/target/arch/amdgpu/lower/descriptor_ref.h"
#include "loom/target/arch/amdgpu/lower/emit.h"
#include "loom/target/arch/amdgpu/lower/materializers.h"
#include "loom/target/arch/amdgpu/lower/types.h"
#include "loom/target/arch/amdgpu/refs/target_refs.h"

typedef enum loom_amdgpu_f64_narrow_format_flag_bits_e {
  LOOM_AMDGPU_F64_NARROW_FORMAT_FLAG_NONE = 0u,
  LOOM_AMDGPU_F64_NARROW_FORMAT_FLAG_PRESERVE_NAN_PAYLOAD = 1u << 0,
} loom_amdgpu_f64_narrow_format_flag_bits_t;
typedef uint8_t loom_amdgpu_f64_narrow_format_flags_t;

typedef struct loom_amdgpu_f64_narrow_format_t {
  // Destination width including sign, exponent, and mantissa fields.
  uint8_t result_bit_count;
  // Number of explicitly stored destination mantissa bits.
  uint8_t mantissa_bit_count;
  // Source exponent encoding at which destination normals begin.
  uint16_t minimum_normal_exponent;
  // Constant term in the subnormal guard-bit position calculation.
  uint16_t subnormal_guard_position_base;
  // First rounded unsigned payload requiring overflow handling.
  uint16_t clamp_threshold;
  // Unsigned payload selected when a finite source overflows.
  uint16_t clamp_value;
  // Destination NaN payload before inserting source payload bits.
  uint16_t nan_payload;
  // Destination-specific exceptional-value behavior.
  loom_amdgpu_f64_narrow_format_flags_t flags;
} loom_amdgpu_f64_narrow_format_t;

static const loom_amdgpu_f64_narrow_format_t kAmdgpuF64NarrowFormats[] = {
    [LOOM_AMDGPU_F64_NARROW_KIND_BF16] =
        {
            .result_bit_count = 16,
            .mantissa_bit_count = 7,
            .minimum_normal_exponent = 897,
            .subnormal_guard_position_base = 909,
            .clamp_threshold = UINT16_C(0x7F80),
            .clamp_value = UINT16_C(0x7F80),
            .nan_payload = UINT16_C(0x7FC0),
            .flags = LOOM_AMDGPU_F64_NARROW_FORMAT_FLAG_PRESERVE_NAN_PAYLOAD,
        },
    [LOOM_AMDGPU_F64_NARROW_KIND_F8E4M3] =
        {
            .result_bit_count = 8,
            .mantissa_bit_count = 3,
            .minimum_normal_exponent = 1017,
            .subnormal_guard_position_base = 1033,
            .clamp_threshold = UINT16_C(0x007F),
            .clamp_value = UINT16_C(0x007E),
            .nan_payload = UINT16_C(0x007F),
            .flags = LOOM_AMDGPU_F64_NARROW_FORMAT_FLAG_NONE,
        },
    [LOOM_AMDGPU_F64_NARROW_KIND_F8E5M2] =
        {
            .result_bit_count = 8,
            .mantissa_bit_count = 2,
            .minimum_normal_exponent = 1009,
            .subnormal_guard_position_base = 1026,
            .clamp_threshold = UINT16_C(0x007C),
            .clamp_value = UINT16_C(0x007C),
            .nan_payload = UINT16_C(0x007F),
            .flags = LOOM_AMDGPU_F64_NARROW_FORMAT_FLAG_NONE,
        },
};
static_assert(IREE_ARRAYSIZE(kAmdgpuF64NarrowFormats) ==
                  LOOM_AMDGPU_F64_NARROW_KIND_COUNT_,
              "F64 narrowing format table must cover every plan kind");

static const loom_amdgpu_descriptor_ref_t
    kAmdgpuF64NarrowRequiredDescriptorRefs[] = {
        LOOM_AMDGPU_DESCRIPTOR_REF_V_MOV_B32,
        LOOM_AMDGPU_DESCRIPTOR_REF_V_MOV_B32_COPY,
        LOOM_AMDGPU_DESCRIPTOR_REF_V_ADD_U32,
        LOOM_AMDGPU_DESCRIPTOR_REF_V_ADD_U32_LIT,
        LOOM_AMDGPU_DESCRIPTOR_REF_V_SUB_U32,
        LOOM_AMDGPU_DESCRIPTOR_REF_V_MIN_U32,
        LOOM_AMDGPU_DESCRIPTOR_REF_V_AND_B32_LIT,
        LOOM_AMDGPU_DESCRIPTOR_REF_V_OR_B32,
        LOOM_AMDGPU_DESCRIPTOR_REF_V_OR_B32_LIT,
        LOOM_AMDGPU_DESCRIPTOR_REF_V_LSHLREV_B32,
        LOOM_AMDGPU_DESCRIPTOR_REF_V_LSHLREV_B32_LIT,
        LOOM_AMDGPU_DESCRIPTOR_REF_V_LSHRREV_B32,
        LOOM_AMDGPU_DESCRIPTOR_REF_V_LSHRREV_B32_LIT,
        LOOM_AMDGPU_DESCRIPTOR_REF_V_CMP_EQ_I32,
        LOOM_AMDGPU_DESCRIPTOR_REF_V_CMP_NE_I32_SRC1_INLINE,
        LOOM_AMDGPU_DESCRIPTOR_REF_V_CMP_UGE_U32,
        LOOM_AMDGPU_DESCRIPTOR_REF_V_CMP_ULT_U32,
        LOOM_AMDGPU_DESCRIPTOR_REF_V_CNDMASK_B32,
        LOOM_AMDGPU_DESCRIPTOR_REF_S_AND_B64,
};

static loom_amdgpu_f64_narrow_kind_t loom_amdgpu_f64_narrow_kind(
    loom_scalar_type_t result_type) {
  switch (result_type) {
    case LOOM_SCALAR_TYPE_BF16:
      return LOOM_AMDGPU_F64_NARROW_KIND_BF16;
    case LOOM_SCALAR_TYPE_F8E4M3:
      return LOOM_AMDGPU_F64_NARROW_KIND_F8E4M3;
    case LOOM_SCALAR_TYPE_F8E5M2:
      return LOOM_AMDGPU_F64_NARROW_KIND_F8E5M2;
    default:
      return LOOM_AMDGPU_F64_NARROW_KIND_NONE;
  }
}

bool loom_amdgpu_select_f64_narrow_plan(
    const loom_low_descriptor_set_t* descriptor_set,
    loom_scalar_type_t source_type, loom_scalar_type_t result_type,
    loom_amdgpu_f64_narrow_plan_t* out_plan) {
  *out_plan = (loom_amdgpu_f64_narrow_plan_t){0};
  if (source_type != LOOM_SCALAR_TYPE_F64) {
    return false;
  }
  const loom_amdgpu_f64_narrow_kind_t kind =
      loom_amdgpu_f64_narrow_kind(result_type);
  if (kind == LOOM_AMDGPU_F64_NARROW_KIND_NONE ||
      !loom_amdgpu_descriptor_set_has_all_refs(
          descriptor_set, kAmdgpuF64NarrowRequiredDescriptorRefs,
          IREE_ARRAYSIZE(kAmdgpuF64NarrowRequiredDescriptorRefs))) {
    return false;
  }
  *out_plan = (loom_amdgpu_f64_narrow_plan_t){.kind = kind};
  return true;
}

static iree_status_t loom_amdgpu_emit_f64_narrow_compare_zero(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    loom_value_id_t value, loom_type_t vgpr_type, loom_type_t mask_type,
    loom_value_id_t* out_mask) {
  return loom_amdgpu_emit_vgpr_compare_immediate(
      context, source_op, LOOM_AMDGPU_DESCRIPTOR_REF_V_CMP_NE_I32,
      LOOM_AMDGPU_DESCRIPTOR_REF_V_CMP_NE_I32_SRC1_INLINE, value, 0, vgpr_type,
      mask_type, out_mask);
}

iree_status_t loom_amdgpu_emit_f64_narrow(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    loom_value_id_t source, loom_value_id_t result,
    const loom_amdgpu_f64_narrow_plan_t* plan) {
  const loom_amdgpu_f64_narrow_format_t* format =
      &kAmdgpuF64NarrowFormats[plan->kind];
  loom_type_t vgpr_type = loom_type_none();
  IREE_RETURN_IF_ERROR(loom_amdgpu_make_vgpr_type(context, &vgpr_type));
  loom_type_t mask_type = loom_type_none();
  IREE_RETURN_IF_ERROR(
      loom_amdgpu_make_sgpr_range_type(context, 2, &mask_type));

  loom_value_id_t low_source = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_amdgpu_lookup_or_materialize_vgpr_registers(
      context, source_op, source, &low_source));
  loom_value_id_t source_low = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_amdgpu_extract_low_register_unit(
      context, source_op, low_source, /*register_count=*/2,
      /*register_offset=*/0, vgpr_type, &source_low));
  loom_value_id_t source_high = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_amdgpu_extract_low_register_unit(
      context, source_op, low_source, /*register_count=*/2,
      /*register_offset=*/1, vgpr_type, &source_high));

  // F64 destinations in this family retain at most seven fraction bits. Every
  // rounding guard therefore lives in the high source word; the low word only
  // contributes sticky information. This avoids constructing or operating on
  // general two-word integer temporaries.
  loom_value_id_t magnitude_high = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_amdgpu_emit_vgpr_binary_immediate(
      context, source_op, LOOM_AMDGPU_DESCRIPTOR_REF_V_AND_B32_LIT, source_high,
      UINT32_C(0x7FFFFFFF), vgpr_type, &magnitude_high));
  loom_value_id_t fraction_high = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_amdgpu_emit_vgpr_binary_immediate(
      context, source_op, LOOM_AMDGPU_DESCRIPTOR_REF_V_AND_B32_LIT,
      magnitude_high, UINT32_C(0x000FFFFF), vgpr_type, &fraction_high));
  loom_value_id_t exponent = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_amdgpu_emit_vgpr_shift(
      context, source_op, LOOM_AMDGPU_DESCRIPTOR_REF_V_LSHRREV_B32_LIT, 20,
      magnitude_high, vgpr_type, &exponent));
  loom_value_id_t significand_high = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_amdgpu_emit_vgpr_binary_immediate(
      context, source_op, LOOM_AMDGPU_DESCRIPTOR_REF_V_OR_B32_LIT,
      fraction_high, UINT32_C(0x00100000), vgpr_type, &significand_high));

  loom_value_id_t minimum_normal_exponent = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_amdgpu_emit_const_u32(
      context, source_op, LOOM_AMDGPU_DESCRIPTOR_REF_V_MOV_B32,
      format->minimum_normal_exponent, vgpr_type, &minimum_normal_exponent));
  loom_value_id_t is_subnormal = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_amdgpu_emit_binary(
      context, source_op, LOOM_AMDGPU_DESCRIPTOR_REF_V_CMP_ULT_U32, exponent,
      minimum_normal_exponent, mask_type, &is_subnormal));

  loom_value_id_t subnormal_guard_base = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_amdgpu_emit_const_u32(
      context, source_op, LOOM_AMDGPU_DESCRIPTOR_REF_V_MOV_B32,
      format->subnormal_guard_position_base, vgpr_type, &subnormal_guard_base));
  loom_value_id_t subnormal_guard_position = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_amdgpu_emit_binary(
      context, source_op, LOOM_AMDGPU_DESCRIPTOR_REF_V_SUB_U32,
      subnormal_guard_base, exponent, vgpr_type, &subnormal_guard_position));
  loom_value_id_t maximum_guard_position = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_amdgpu_emit_const_u32(
      context, source_op, LOOM_AMDGPU_DESCRIPTOR_REF_V_MOV_B32, 30, vgpr_type,
      &maximum_guard_position));
  IREE_RETURN_IF_ERROR(loom_amdgpu_emit_binary(
      context, source_op, LOOM_AMDGPU_DESCRIPTOR_REF_V_MIN_U32,
      subnormal_guard_position, maximum_guard_position, vgpr_type,
      &subnormal_guard_position));
  loom_value_id_t normal_guard_position = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_amdgpu_emit_const_u32(
      context, source_op, LOOM_AMDGPU_DESCRIPTOR_REF_V_MOV_B32,
      19u - format->mantissa_bit_count, vgpr_type, &normal_guard_position));
  loom_value_id_t guard_position = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_amdgpu_emit_vgpr_select(
      context, source_op, normal_guard_position, subnormal_guard_position,
      is_subnormal, vgpr_type, &guard_position));

  loom_value_id_t head = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_amdgpu_emit_binary(
      context, source_op, LOOM_AMDGPU_DESCRIPTOR_REF_V_LSHRREV_B32,
      guard_position, significand_high, vgpr_type, &head));
  loom_value_id_t truncated = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_amdgpu_emit_vgpr_shift(
      context, source_op, LOOM_AMDGPU_DESCRIPTOR_REF_V_LSHRREV_B32_LIT, 1, head,
      vgpr_type, &truncated));
  loom_value_id_t guard = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_amdgpu_emit_vgpr_binary_immediate(
      context, source_op, LOOM_AMDGPU_DESCRIPTOR_REF_V_AND_B32_LIT, head, 1,
      vgpr_type, &guard));

  loom_value_id_t word_bit_count = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_amdgpu_emit_const_u32(
      context, source_op, LOOM_AMDGPU_DESCRIPTOR_REF_V_MOV_B32, 32, vgpr_type,
      &word_bit_count));
  loom_value_id_t sticky_shift = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_amdgpu_emit_binary(
      context, source_op, LOOM_AMDGPU_DESCRIPTOR_REF_V_SUB_U32, word_bit_count,
      guard_position, vgpr_type, &sticky_shift));
  loom_value_id_t sticky_high = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_amdgpu_emit_binary(
      context, source_op, LOOM_AMDGPU_DESCRIPTOR_REF_V_LSHLREV_B32,
      sticky_shift, significand_high, vgpr_type, &sticky_high));
  loom_value_id_t sticky = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_amdgpu_emit_binary(
      context, source_op, LOOM_AMDGPU_DESCRIPTOR_REF_V_OR_B32, sticky_high,
      source_low, vgpr_type, &sticky));
  loom_value_id_t retained_lsb = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_amdgpu_emit_vgpr_binary_immediate(
      context, source_op, LOOM_AMDGPU_DESCRIPTOR_REF_V_AND_B32_LIT, truncated,
      1, vgpr_type, &retained_lsb));
  loom_value_id_t round_evidence = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_amdgpu_emit_binary(
      context, source_op, LOOM_AMDGPU_DESCRIPTOR_REF_V_OR_B32, sticky,
      retained_lsb, vgpr_type, &round_evidence));
  loom_value_id_t has_round_evidence = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_amdgpu_emit_f64_narrow_compare_zero(
      context, source_op, round_evidence, vgpr_type, mask_type,
      &has_round_evidence));
  loom_value_id_t zero = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_amdgpu_emit_const_u32(
      context, source_op, LOOM_AMDGPU_DESCRIPTOR_REF_V_MOV_B32, 0, vgpr_type,
      &zero));
  loom_value_id_t increment = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_amdgpu_emit_vgpr_select(context, source_op, zero,
                                                    guard, has_round_evidence,
                                                    vgpr_type, &increment));
  loom_value_id_t rounded = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_amdgpu_emit_binary(
      context, source_op, LOOM_AMDGPU_DESCRIPTOR_REF_V_ADD_U32, truncated,
      increment, vgpr_type, &rounded));

  loom_value_id_t normal_exponent = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_amdgpu_emit_vgpr_binary_immediate(
      context, source_op, LOOM_AMDGPU_DESCRIPTOR_REF_V_ADD_U32_LIT, exponent,
      0u - format->minimum_normal_exponent, vgpr_type, &normal_exponent));
  loom_value_id_t normal_base = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_amdgpu_emit_vgpr_shift(
      context, source_op, LOOM_AMDGPU_DESCRIPTOR_REF_V_LSHLREV_B32_LIT,
      format->mantissa_bit_count, normal_exponent, vgpr_type, &normal_base));
  loom_value_id_t selected_base = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(
      loom_amdgpu_emit_vgpr_select(context, source_op, normal_base, zero,
                                   is_subnormal, vgpr_type, &selected_base));
  loom_value_id_t finite = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_amdgpu_emit_binary(
      context, source_op, LOOM_AMDGPU_DESCRIPTOR_REF_V_ADD_U32, rounded,
      selected_base, vgpr_type, &finite));

  loom_value_id_t clamp_threshold = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_amdgpu_emit_const_u32(
      context, source_op, LOOM_AMDGPU_DESCRIPTOR_REF_V_MOV_B32,
      format->clamp_threshold, vgpr_type, &clamp_threshold));
  loom_value_id_t needs_clamp = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_amdgpu_emit_binary(
      context, source_op, LOOM_AMDGPU_DESCRIPTOR_REF_V_CMP_UGE_U32, finite,
      clamp_threshold, mask_type, &needs_clamp));
  loom_value_id_t clamp_value = clamp_threshold;
  if (format->clamp_value != format->clamp_threshold) {
    IREE_RETURN_IF_ERROR(loom_amdgpu_emit_const_u32(
        context, source_op, LOOM_AMDGPU_DESCRIPTOR_REF_V_MOV_B32,
        format->clamp_value, vgpr_type, &clamp_value));
  }
  IREE_RETURN_IF_ERROR(loom_amdgpu_emit_vgpr_select(context, source_op, finite,
                                                    clamp_value, needs_clamp,
                                                    vgpr_type, &finite));

  loom_value_id_t source_fraction = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_amdgpu_emit_binary(
      context, source_op, LOOM_AMDGPU_DESCRIPTOR_REF_V_OR_B32, fraction_high,
      source_low, vgpr_type, &source_fraction));
  loom_value_id_t has_source_fraction = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_amdgpu_emit_f64_narrow_compare_zero(
      context, source_op, source_fraction, vgpr_type, mask_type,
      &has_source_fraction));
  loom_value_id_t special_exponent = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_amdgpu_emit_const_u32(
      context, source_op, LOOM_AMDGPU_DESCRIPTOR_REF_V_MOV_B32, 2047, vgpr_type,
      &special_exponent));
  loom_value_id_t has_special_exponent = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_amdgpu_emit_binary(
      context, source_op, LOOM_AMDGPU_DESCRIPTOR_REF_V_CMP_EQ_I32, exponent,
      special_exponent, mask_type, &has_special_exponent));
  loom_value_id_t is_nan = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_amdgpu_emit_binary(
      context, source_op, LOOM_AMDGPU_DESCRIPTOR_REF_S_AND_B64,
      has_source_fraction, has_special_exponent, mask_type, &is_nan));

  loom_value_id_t nan = LOOM_VALUE_ID_INVALID;
  if (iree_any_bit_set(
          format->flags,
          LOOM_AMDGPU_F64_NARROW_FORMAT_FLAG_PRESERVE_NAN_PAYLOAD)) {
    IREE_RETURN_IF_ERROR(loom_amdgpu_emit_vgpr_shift(
        context, source_op, LOOM_AMDGPU_DESCRIPTOR_REF_V_LSHRREV_B32_LIT,
        20u - format->mantissa_bit_count, fraction_high, vgpr_type, &nan));
    IREE_RETURN_IF_ERROR(loom_amdgpu_emit_vgpr_binary_immediate(
        context, source_op, LOOM_AMDGPU_DESCRIPTOR_REF_V_OR_B32_LIT, nan,
        format->nan_payload, vgpr_type, &nan));
  } else {
    IREE_RETURN_IF_ERROR(loom_amdgpu_emit_const_u32(
        context, source_op, LOOM_AMDGPU_DESCRIPTOR_REF_V_MOV_B32,
        format->nan_payload, vgpr_type, &nan));
  }
  loom_value_id_t unsigned_result = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_amdgpu_emit_vgpr_select(
      context, source_op, finite, nan, is_nan, vgpr_type, &unsigned_result));

  loom_value_id_t sign = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_amdgpu_emit_vgpr_shift(
      context, source_op, LOOM_AMDGPU_DESCRIPTOR_REF_V_LSHRREV_B32_LIT, 31,
      source_high, vgpr_type, &sign));
  IREE_RETURN_IF_ERROR(loom_amdgpu_emit_vgpr_shift(
      context, source_op, LOOM_AMDGPU_DESCRIPTOR_REF_V_LSHLREV_B32_LIT,
      format->result_bit_count - 1u, sign, vgpr_type, &sign));
  loom_value_id_t low_result = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_amdgpu_emit_binary(
      context, source_op, LOOM_AMDGPU_DESCRIPTOR_REF_V_OR_B32, sign,
      unsigned_result, vgpr_type, &low_result));
  return loom_low_lower_bind_value(context, result, low_result);
}
