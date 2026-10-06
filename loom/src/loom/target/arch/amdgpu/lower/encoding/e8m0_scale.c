// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/target/arch/amdgpu/lower/encoding/e8m0_scale.h"

#include "loom/target/arch/amdgpu/lower/descriptor_ref.h"
#include "loom/target/arch/amdgpu/lower/emit.h"
#include "loom/target/arch/amdgpu/lower/types.h"
#include "loom/target/arch/amdgpu/refs/target_refs.h"

enum {
  // Mask isolating one packed E8M0 byte.
  LOOM_AMDGPU_E8M0_SCALE_BYTE_MASK = 0xFFu,
  // IEEE F32 exponent position receiving the encoded E8M0 byte.
  LOOM_AMDGPU_E8M0_F32_EXPONENT_SHIFT = 23u,
  // Exact F32 encoding of the minimum E8M0 value, 2^-127.
  LOOM_AMDGPU_E8M0_MINIMUM_F32_BITS = 0x00400000u,
  // Canonical quiet-NaN F32 encoding used for E8M0 byte 255.
  LOOM_AMDGPU_E8M0_QUIET_NAN_F32_BITS = 0x7FC00000u,
};

bool loom_amdgpu_e8m0_f32_scale_materialization_available(
    const loom_low_descriptor_set_t* descriptor_set) {
  return descriptor_set != NULL &&
         loom_amdgpu_descriptor_set_can_emit_vgpr_binary_immediate(
             descriptor_set, LOOM_AMDGPU_DESCRIPTOR_REF_V_LSHRREV_B32_LIT,
             24u) &&
         loom_amdgpu_descriptor_set_can_emit_vgpr_binary_immediate(
             descriptor_set, LOOM_AMDGPU_DESCRIPTOR_REF_V_AND_B32_LIT,
             LOOM_AMDGPU_E8M0_SCALE_BYTE_MASK) &&
         loom_amdgpu_descriptor_set_can_emit_vgpr_binary_immediate(
             descriptor_set, LOOM_AMDGPU_DESCRIPTOR_REF_V_LSHLREV_B32_LIT,
             LOOM_AMDGPU_E8M0_F32_EXPONENT_SHIFT) &&
         loom_amdgpu_descriptor_set_can_emit_vgpr_compare_immediate(
             descriptor_set, LOOM_AMDGPU_DESCRIPTOR_REF_V_CMP_EQ_I32,
             LOOM_AMDGPU_DESCRIPTOR_REF_V_CMP_EQ_I32_SRC1_INLINE, 0u) &&
         loom_amdgpu_descriptor_set_can_emit_vgpr_compare_immediate(
             descriptor_set, LOOM_AMDGPU_DESCRIPTOR_REF_V_CMP_EQ_I32,
             LOOM_AMDGPU_DESCRIPTOR_REF_V_CMP_EQ_I32_SRC1_INLINE,
             LOOM_AMDGPU_E8M0_SCALE_BYTE_MASK) &&
         loom_amdgpu_descriptor_set_has_ref(
             descriptor_set, LOOM_AMDGPU_DESCRIPTOR_REF_V_CNDMASK_B32) &&
         loom_amdgpu_descriptor_set_has_ref(
             descriptor_set, LOOM_AMDGPU_DESCRIPTOR_REF_V_MOV_B32);
}

iree_status_t loom_amdgpu_initialize_e8m0_f32_scale_materializer(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    loom_amdgpu_e8m0_f32_scale_materializer_t* out_materializer) {
  *out_materializer = (loom_amdgpu_e8m0_f32_scale_materializer_t){0};
  IREE_RETURN_IF_ERROR(
      loom_amdgpu_make_vgpr_type(context, &out_materializer->vector_type));
  IREE_RETURN_IF_ERROR(loom_amdgpu_make_sgpr_range_type(
      context, 2u, &out_materializer->mask_type));
  IREE_RETURN_IF_ERROR(loom_amdgpu_emit_const_u32(
      context, source_op, LOOM_AMDGPU_DESCRIPTOR_REF_V_MOV_B32,
      LOOM_AMDGPU_E8M0_MINIMUM_F32_BITS, out_materializer->vector_type,
      &out_materializer->low_minimum_scale));
  return loom_amdgpu_emit_const_u32(
      context, source_op, LOOM_AMDGPU_DESCRIPTOR_REF_V_MOV_B32,
      LOOM_AMDGPU_E8M0_QUIET_NAN_F32_BITS, out_materializer->vector_type,
      &out_materializer->low_quiet_nan_scale);
}

iree_status_t loom_amdgpu_materialize_e8m0_f32_scale(
    loom_low_lower_context_t* context, const loom_op_t* source_op,
    const loom_amdgpu_e8m0_f32_scale_materializer_t* materializer,
    loom_value_id_t low_scale_source, uint32_t scale_register_count,
    uint32_t scale_index, loom_value_id_t* out_low_f32_scale) {
  IREE_ASSERT_LT(scale_index, scale_register_count *
                                  LOOM_AMDGPU_E8M0_SCALE_VALUES_PER_REGISTER);
  const uint32_t source_register_index =
      scale_index / LOOM_AMDGPU_E8M0_SCALE_VALUES_PER_REGISTER;
  const uint32_t source_byte_index =
      scale_index % LOOM_AMDGPU_E8M0_SCALE_VALUES_PER_REGISTER;
  loom_value_id_t low_source_register = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_amdgpu_extract_low_register_unit(
      context, source_op, low_scale_source, scale_register_count,
      source_register_index, materializer->vector_type, &low_source_register));

  loom_value_id_t low_scale_byte = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_amdgpu_emit_vgpr_shift(
      context, source_op, LOOM_AMDGPU_DESCRIPTOR_REF_V_LSHRREV_B32_LIT,
      source_byte_index * 8u, low_source_register, materializer->vector_type,
      &low_scale_byte));
  IREE_RETURN_IF_ERROR(loom_amdgpu_emit_vgpr_binary_immediate(
      context, source_op, LOOM_AMDGPU_DESCRIPTOR_REF_V_AND_B32_LIT,
      low_scale_byte, LOOM_AMDGPU_E8M0_SCALE_BYTE_MASK,
      materializer->vector_type, &low_scale_byte));

  loom_value_id_t low_exponent_bits = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_amdgpu_emit_vgpr_shift(
      context, source_op, LOOM_AMDGPU_DESCRIPTOR_REF_V_LSHLREV_B32_LIT,
      LOOM_AMDGPU_E8M0_F32_EXPONENT_SHIFT, low_scale_byte,
      materializer->vector_type, &low_exponent_bits));
  loom_value_id_t low_is_minimum = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_amdgpu_emit_vgpr_compare_immediate(
      context, source_op, LOOM_AMDGPU_DESCRIPTOR_REF_V_CMP_EQ_I32,
      LOOM_AMDGPU_DESCRIPTOR_REF_V_CMP_EQ_I32_SRC1_INLINE, low_scale_byte, 0u,
      materializer->vector_type, materializer->mask_type, &low_is_minimum));
  loom_value_id_t low_finite_scale = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_amdgpu_emit_vgpr_select(
      context, source_op, low_exponent_bits, materializer->low_minimum_scale,
      low_is_minimum, materializer->vector_type, &low_finite_scale));

  loom_value_id_t low_is_nan = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_amdgpu_emit_vgpr_compare_immediate(
      context, source_op, LOOM_AMDGPU_DESCRIPTOR_REF_V_CMP_EQ_I32,
      LOOM_AMDGPU_DESCRIPTOR_REF_V_CMP_EQ_I32_SRC1_INLINE, low_scale_byte,
      LOOM_AMDGPU_E8M0_SCALE_BYTE_MASK, materializer->vector_type,
      materializer->mask_type, &low_is_nan));
  return loom_amdgpu_emit_vgpr_select(
      context, source_op, low_finite_scale, materializer->low_quiet_nan_scale,
      low_is_nan, materializer->vector_type, out_low_f32_scale);
}
