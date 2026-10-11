// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/target/arch/amdgpu/lower/encoding/float_to_integer64.h"

#include <stdint.h>

#include "loom/target/arch/amdgpu/lower/descriptor_ref.h"
#include "loom/target/arch/amdgpu/lower/emit.h"
#include "loom/target/arch/amdgpu/lower/types.h"

static const loom_amdgpu_descriptor_ref_t
    kAmdgpuFloat32ToInteger64RequiredDescriptorRefs[] = {
        LOOM_AMDGPU_DESCRIPTOR_REF_S_MOV_B32,
        LOOM_AMDGPU_DESCRIPTOR_REF_V_MOV_B32,
        LOOM_AMDGPU_DESCRIPTOR_REF_V_MOV_B32_COPY,
        LOOM_AMDGPU_DESCRIPTOR_REF_V_TRUNC_F32,
        LOOM_AMDGPU_DESCRIPTOR_REF_V_LDEXP_F32,
        LOOM_AMDGPU_DESCRIPTOR_REF_V_FLOOR_F32,
        LOOM_AMDGPU_DESCRIPTOR_REF_V_FMA_F32,
        LOOM_AMDGPU_DESCRIPTOR_REF_V_CVT_U32_F32,
};

static const loom_amdgpu_descriptor_ref_t
    kAmdgpuSignedFloat32ToInteger64RequiredDescriptorRefs[] = {
        LOOM_AMDGPU_DESCRIPTOR_REF_V_AND_B32_LIT,
        LOOM_AMDGPU_DESCRIPTOR_REF_V_ASHRREV_I32_LIT,
        LOOM_AMDGPU_DESCRIPTOR_REF_V_XOR_B32,
        LOOM_AMDGPU_DESCRIPTOR_REF_V_SUB_CO_U32,
        LOOM_AMDGPU_DESCRIPTOR_REF_V_SUB_CO_CI_U32,
};

static const loom_amdgpu_descriptor_ref_t
    kAmdgpuBfloat16ToInteger64RequiredDescriptorRefs[] = {
        LOOM_AMDGPU_DESCRIPTOR_REF_V_AND_B32_LIT,
        LOOM_AMDGPU_DESCRIPTOR_REF_V_LSHLREV_B32_LIT,
};

static const loom_amdgpu_descriptor_ref_t
    kAmdgpuFloat64ToInteger64RequiredDescriptorRefs[] = {
        LOOM_AMDGPU_DESCRIPTOR_REF_S_MOV_B32,
        LOOM_AMDGPU_DESCRIPTOR_REF_V_MOV_B32_COPY,
        LOOM_AMDGPU_DESCRIPTOR_REF_V_TRUNC_F64,
        LOOM_AMDGPU_DESCRIPTOR_REF_V_LDEXP_F64,
        LOOM_AMDGPU_DESCRIPTOR_REF_V_FLOOR_F64,
        LOOM_AMDGPU_DESCRIPTOR_REF_V_FMA_F64,
        LOOM_AMDGPU_DESCRIPTOR_REF_V_CVT_U32_F64,
};

static const loom_amdgpu_descriptor_ref_t
    kAmdgpuSignedFloat64ToInteger64RequiredDescriptorRefs[] = {
        LOOM_AMDGPU_DESCRIPTOR_REF_V_CVT_I32_F64,
};

bool loom_amdgpu_select_float_to_integer64_plan(
    const loom_low_descriptor_set_t* descriptor_set,
    loom_scalar_type_t source_type,
    loom_amdgpu_float_to_integer64_signedness_t signedness,
    loom_amdgpu_float_to_integer64_plan_t* out_plan) {
  *out_plan = (loom_amdgpu_float_to_integer64_plan_t){0};
  if (signedness != LOOM_AMDGPU_FLOAT_TO_INTEGER64_SIGNEDNESS_SIGNED &&
      signedness != LOOM_AMDGPU_FLOAT_TO_INTEGER64_SIGNEDNESS_UNSIGNED) {
    return false;
  }

  const bool is_signed =
      signedness == LOOM_AMDGPU_FLOAT_TO_INTEGER64_SIGNEDNESS_SIGNED;
  if (source_type == LOOM_SCALAR_TYPE_BF16 ||
      source_type == LOOM_SCALAR_TYPE_F32) {
    if (!loom_amdgpu_descriptor_set_has_all_refs(
            descriptor_set, kAmdgpuFloat32ToInteger64RequiredDescriptorRefs,
            IREE_ARRAYSIZE(kAmdgpuFloat32ToInteger64RequiredDescriptorRefs)) ||
        (is_signed &&
         !loom_amdgpu_descriptor_set_has_all_refs(
             descriptor_set,
             kAmdgpuSignedFloat32ToInteger64RequiredDescriptorRefs,
             IREE_ARRAYSIZE(
                 kAmdgpuSignedFloat32ToInteger64RequiredDescriptorRefs))) ||
        (source_type == LOOM_SCALAR_TYPE_BF16 &&
         !loom_amdgpu_descriptor_set_has_all_refs(
             descriptor_set, kAmdgpuBfloat16ToInteger64RequiredDescriptorRefs,
             IREE_ARRAYSIZE(
                 kAmdgpuBfloat16ToInteger64RequiredDescriptorRefs)))) {
      return false;
    }
    out_plan->kind = is_signed
                         ? LOOM_AMDGPU_FLOAT_TO_INTEGER64_KIND_F32_SIGNED
                         : LOOM_AMDGPU_FLOAT_TO_INTEGER64_KIND_F32_UNSIGNED;
    return true;
  }

  if (source_type != LOOM_SCALAR_TYPE_F64 ||
      !loom_amdgpu_descriptor_set_has_all_refs(
          descriptor_set, kAmdgpuFloat64ToInteger64RequiredDescriptorRefs,
          IREE_ARRAYSIZE(kAmdgpuFloat64ToInteger64RequiredDescriptorRefs)) ||
      (is_signed &&
       !loom_amdgpu_descriptor_set_has_all_refs(
           descriptor_set,
           kAmdgpuSignedFloat64ToInteger64RequiredDescriptorRefs,
           IREE_ARRAYSIZE(
               kAmdgpuSignedFloat64ToInteger64RequiredDescriptorRefs)))) {
    return false;
  }
  out_plan->kind = is_signed ? LOOM_AMDGPU_FLOAT_TO_INTEGER64_KIND_F64_SIGNED
                             : LOOM_AMDGPU_FLOAT_TO_INTEGER64_KIND_F64_UNSIGNED;
  return true;
}

static bool loom_amdgpu_float_to_integer64_kind_is_f32(
    loom_amdgpu_float_to_integer64_kind_t kind) {
  return kind == LOOM_AMDGPU_FLOAT_TO_INTEGER64_KIND_F32_SIGNED ||
         kind == LOOM_AMDGPU_FLOAT_TO_INTEGER64_KIND_F32_UNSIGNED;
}

iree_status_t loom_amdgpu_initialize_float_to_integer64_emission(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    const loom_amdgpu_float_to_integer64_plan_t* plan,
    loom_amdgpu_float_to_integer64_emission_t* out_emission) {
  *out_emission = (loom_amdgpu_float_to_integer64_emission_t){0};
  IREE_RETURN_IF_ERROR(
      loom_amdgpu_make_vgpr_type(context, &out_emission->word_type));
  IREE_RETURN_IF_ERROR(
      loom_amdgpu_make_vgpr_range_type(context, 2, &out_emission->pair_type));
  const bool is_f32 = loom_amdgpu_float_to_integer64_kind_is_f32(plan->kind);
  out_emission->floating_type =
      is_f32 ? out_emission->word_type : out_emission->pair_type;

  loom_type_t sgpr_type = loom_type_none();
  IREE_RETURN_IF_ERROR(loom_amdgpu_make_sgpr_type(context, &sgpr_type));
  IREE_RETURN_IF_ERROR(loom_amdgpu_emit_const_u32(
      context, source_op, LOOM_AMDGPU_DESCRIPTOR_REF_S_MOV_B32,
      UINT32_C(0xFFFFFFE0), sgpr_type, &out_emission->split_exponent));
  if (is_f32) {
    return loom_amdgpu_emit_const_u32(
        context, source_op, LOOM_AMDGPU_DESCRIPTOR_REF_S_MOV_B32,
        UINT32_C(0xCF800000), sgpr_type, &out_emission->negative_word_scale);
  }

  loom_value_id_t scale_words[2] = {
      LOOM_VALUE_ID_INVALID,
      LOOM_VALUE_ID_INVALID,
  };
  IREE_RETURN_IF_ERROR(loom_amdgpu_emit_const_u32(
      context, source_op, LOOM_AMDGPU_DESCRIPTOR_REF_S_MOV_B32, 0, sgpr_type,
      &scale_words[0]));
  IREE_RETURN_IF_ERROR(loom_amdgpu_emit_const_u32(
      context, source_op, LOOM_AMDGPU_DESCRIPTOR_REF_S_MOV_B32,
      UINT32_C(0xC1F00000), sgpr_type, &scale_words[1]));
  loom_type_t sgpr_pair_type = loom_type_none();
  IREE_RETURN_IF_ERROR(
      loom_amdgpu_make_sgpr_range_type(context, 2, &sgpr_pair_type));
  return loom_amdgpu_build_low_register_range(
      context, source_op, scale_words, IREE_ARRAYSIZE(scale_words),
      sgpr_pair_type, &out_emission->negative_word_scale);
}

static iree_status_t loom_amdgpu_emit_float_to_integer64_fma(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    loom_amdgpu_descriptor_ref_t descriptor_ref, loom_value_id_t lhs,
    loom_value_id_t rhs, loom_value_id_t addend, loom_type_t result_type,
    loom_value_id_t* out_result) {
  loom_low_lower_resolved_descriptor_t descriptor = {0};
  IREE_RETURN_IF_ERROR(
      loom_amdgpu_resolve_descriptor_ref(context, descriptor_ref, &descriptor));
  return loom_amdgpu_emit_resolved_vgpr_ternary(context, source_op, &descriptor,
                                                lhs, rhs, addend, result_type,
                                                out_result);
}

static iree_status_t loom_amdgpu_emit_float_to_integer64_words(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    const loom_amdgpu_float_to_integer64_plan_t* plan,
    const loom_amdgpu_float_to_integer64_emission_t* emission,
    loom_value_id_t low_source, loom_value_id_t out_words[2]) {
  const bool is_f32 = loom_amdgpu_float_to_integer64_kind_is_f32(plan->kind);
  const loom_amdgpu_descriptor_ref_t truncate_descriptor_ref =
      is_f32 ? LOOM_AMDGPU_DESCRIPTOR_REF_V_TRUNC_F32
             : LOOM_AMDGPU_DESCRIPTOR_REF_V_TRUNC_F64;
  const loom_amdgpu_descriptor_ref_t scale_descriptor_ref =
      is_f32 ? LOOM_AMDGPU_DESCRIPTOR_REF_V_LDEXP_F32
             : LOOM_AMDGPU_DESCRIPTOR_REF_V_LDEXP_F64;
  const loom_amdgpu_descriptor_ref_t floor_descriptor_ref =
      is_f32 ? LOOM_AMDGPU_DESCRIPTOR_REF_V_FLOOR_F32
             : LOOM_AMDGPU_DESCRIPTOR_REF_V_FLOOR_F64;
  const loom_amdgpu_descriptor_ref_t fma_descriptor_ref =
      is_f32 ? LOOM_AMDGPU_DESCRIPTOR_REF_V_FMA_F32
             : LOOM_AMDGPU_DESCRIPTOR_REF_V_FMA_F64;
  const loom_amdgpu_descriptor_ref_t low_convert_descriptor_ref =
      is_f32 ? LOOM_AMDGPU_DESCRIPTOR_REF_V_CVT_U32_F32
             : LOOM_AMDGPU_DESCRIPTOR_REF_V_CVT_U32_F64;
  loom_amdgpu_descriptor_ref_t high_convert_descriptor_ref =
      low_convert_descriptor_ref;
  if (plan->kind == LOOM_AMDGPU_FLOAT_TO_INTEGER64_KIND_F64_SIGNED) {
    high_convert_descriptor_ref = LOOM_AMDGPU_DESCRIPTOR_REF_V_CVT_I32_F64;
  }

  loom_value_id_t truncated = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_amdgpu_emit_vgpr_unary(
      context, source_op, truncate_descriptor_ref, low_source,
      emission->floating_type, &truncated));
  loom_value_id_t scaled = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_amdgpu_emit_binary(
      context, source_op, scale_descriptor_ref, truncated,
      emission->split_exponent, emission->floating_type, &scaled));
  loom_value_id_t high_float = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_amdgpu_emit_vgpr_unary(
      context, source_op, floor_descriptor_ref, scaled, emission->floating_type,
      &high_float));
  loom_value_id_t low_float = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_amdgpu_emit_float_to_integer64_fma(
      context, source_op, fma_descriptor_ref, high_float,
      emission->negative_word_scale, truncated, emission->floating_type,
      &low_float));
  IREE_RETURN_IF_ERROR(loom_amdgpu_emit_vgpr_unary(
      context, source_op, low_convert_descriptor_ref, low_float,
      emission->word_type, &out_words[0]));
  return loom_amdgpu_emit_vgpr_unary(context, source_op,
                                     high_convert_descriptor_ref, high_float,
                                     emission->word_type, &out_words[1]);
}

iree_status_t loom_amdgpu_emit_float_to_integer64(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    const loom_amdgpu_float_to_integer64_plan_t* plan,
    const loom_amdgpu_float_to_integer64_emission_t* emission,
    loom_value_id_t low_source, loom_value_id_t* out_low_result) {
  *out_low_result = LOOM_VALUE_ID_INVALID;
  loom_value_id_t split_source = low_source;
  loom_value_id_t sign = LOOM_VALUE_ID_INVALID;
  if (plan->kind == LOOM_AMDGPU_FLOAT_TO_INTEGER64_KIND_F32_SIGNED) {
    IREE_RETURN_IF_ERROR(loom_amdgpu_emit_vgpr_shift(
        context, source_op, LOOM_AMDGPU_DESCRIPTOR_REF_V_ASHRREV_I32_LIT, 31,
        low_source, emission->word_type, &sign));
    IREE_RETURN_IF_ERROR(loom_amdgpu_emit_vgpr_binary_immediate(
        context, source_op, LOOM_AMDGPU_DESCRIPTOR_REF_V_AND_B32_LIT,
        low_source, UINT32_C(0x7FFFFFFF), emission->word_type, &split_source));
  }

  loom_value_id_t words[2] = {
      LOOM_VALUE_ID_INVALID,
      LOOM_VALUE_ID_INVALID,
  };
  IREE_RETURN_IF_ERROR(loom_amdgpu_emit_float_to_integer64_words(
      context, source_op, plan, emission, split_source, words));
  if (plan->kind != LOOM_AMDGPU_FLOAT_TO_INTEGER64_KIND_F32_SIGNED) {
    return loom_amdgpu_build_low_register_range(
        context, source_op, words, IREE_ARRAYSIZE(words), emission->pair_type,
        out_low_result);
  }

  loom_value_id_t complemented_words[2] = {
      LOOM_VALUE_ID_INVALID,
      LOOM_VALUE_ID_INVALID,
  };
  for (iree_host_size_t i = 0; i < IREE_ARRAYSIZE(words); ++i) {
    IREE_RETURN_IF_ERROR(loom_amdgpu_emit_binary(
        context, source_op, LOOM_AMDGPU_DESCRIPTOR_REF_V_XOR_B32, words[i],
        sign, emission->word_type, &complemented_words[i]));
  }
  loom_value_id_t complemented = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_amdgpu_build_low_register_range(
      context, source_op, complemented_words,
      IREE_ARRAYSIZE(complemented_words), emission->pair_type, &complemented));
  const loom_value_id_t sign_words[2] = {sign, sign};
  loom_value_id_t sign_pair = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_amdgpu_build_low_register_range(
      context, source_op, sign_words, IREE_ARRAYSIZE(sign_words),
      emission->pair_type, &sign_pair));
  return loom_amdgpu_emit_vgpr64_sub(context, source_op, complemented,
                                     sign_pair, out_low_result);
}
