// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// AMDGPU FP8/BF8 vector conversion plan selection and queries.

#ifndef LOOM_TARGET_ARCH_AMDGPU_LOWER_ENCODING_FP8_VECTOR_PLAN_H_
#define LOOM_TARGET_ARCH_AMDGPU_LOWER_ENCODING_FP8_VECTOR_PLAN_H_

#include "loom/codegen/low/lower/lower.h"
#include "loom/target/arch/amdgpu/lower/plan.h"

#ifdef __cplusplus
extern "C" {
#endif

// Packed source geometry for one adjacent pair of logical FP8 lanes.
typedef struct loom_amdgpu_vector_fp8_pair_storage_t {
  // Source register containing the selected adjacent FP8 byte pair.
  uint32_t source_register_index;
  // First FP8 byte offset within the source register.
  uint32_t byte_offset;
  // Number of live logical lanes consumed from this pair.
  uint32_t live_lane_count;
} loom_amdgpu_vector_fp8_pair_storage_t;

// Returns whether |lane_index| begins an adjacent source pair and writes its
// packed storage geometry when present.
static inline bool loom_amdgpu_vector_fp8_query_storage_pair(
    const loom_amdgpu_vector_16bit_float_conversion_plan_t* plan,
    uint32_t lane_index,
    loom_amdgpu_vector_fp8_pair_storage_t* out_pair_storage) {
  *out_pair_storage = (loom_amdgpu_vector_fp8_pair_storage_t){0};
  if (lane_index >= plan->lane_count) {
    return false;
  }

  const uint64_t storage_lane =
      (uint64_t)plan->storage_lane_offset +
      (uint64_t)lane_index * (uint64_t)plan->storage_lane_stride;
  const uint64_t next_storage_lane =
      (uint64_t)plan->storage_lane_offset +
      (uint64_t)(lane_index + 1u) * (uint64_t)plan->storage_lane_stride;
  if (next_storage_lane != storage_lane + 1u ||
      next_storage_lane >= plan->storage_lane_count) {
    return false;
  }

  const uint32_t register_index = (uint32_t)(storage_lane / 4u);
  const uint32_t byte_offset = (uint32_t)(storage_lane % 4u);
  if (byte_offset >= 3u || next_storage_lane / 4u != register_index) {
    return false;
  }
  IREE_ASSERT_LT(register_index, plan->storage_register_count);
  const uint32_t remaining_lane_count = plan->lane_count - lane_index;
  *out_pair_storage = (loom_amdgpu_vector_fp8_pair_storage_t){
      .source_register_index = register_index,
      .byte_offset = byte_offset,
      .live_lane_count = remaining_lane_count < 2u ? remaining_lane_count : 2u,
  };
  return true;
}

// Returns the number of selected actions covering the physical result.
static inline uint32_t loom_amdgpu_vector_fp8_decode_action_count(
    const loom_amdgpu_vector_16bit_float_conversion_plan_t* plan) {
  return plan->result_element_type == LOOM_SCALAR_TYPE_F32
             ? plan->lane_count
             : plan->result_register_count;
}

// Returns whether the plan consumes any explicit scale source.
static inline bool loom_amdgpu_vector_fp8_plan_has_scale(
    const loom_amdgpu_vector_16bit_float_conversion_plan_t* plan) {
  return plan->scale_source != LOOM_VALUE_ID_INVALID;
}

// Returns whether the plan consumes an explicit F32 scale.
static inline bool loom_amdgpu_vector_fp8_plan_has_f32_scale(
    const loom_amdgpu_vector_16bit_float_conversion_plan_t* plan) {
  return loom_amdgpu_vector_fp8_plan_has_scale(plan) &&
         plan->scale_format == LOOM_VALUE_FACT_NUMERIC_FORMAT_F32;
}

// Returns whether the plan consumes packed E8M0 group scales.
static inline bool loom_amdgpu_vector_fp8_plan_has_e8m0_scale(
    const loom_amdgpu_vector_16bit_float_conversion_plan_t* plan) {
  return loom_amdgpu_vector_fp8_plan_has_scale(plan) &&
         plan->scale_format == LOOM_VALUE_FACT_NUMERIC_FORMAT_F8_E8M0;
}

// Returns whether packed E8M0 scales are materialized as F32 operands.
static inline bool loom_amdgpu_vector_fp8_plan_materializes_e8m0_f32_scale(
    const loom_amdgpu_vector_16bit_float_conversion_plan_t* plan) {
  return plan->scale_materialization_kind ==
         LOOM_AMDGPU_VECTOR_SCALE_MATERIALIZATION_E8M0_F32;
}

// Returns whether conversion actions consume F32 scale operands.
static inline bool loom_amdgpu_vector_fp8_plan_uses_f32_scale(
    const loom_amdgpu_vector_16bit_float_conversion_plan_t* plan) {
  return loom_amdgpu_vector_fp8_plan_has_f32_scale(plan) ||
         loom_amdgpu_vector_fp8_plan_materializes_e8m0_f32_scale(plan);
}

// Selects the exact FP8/BF8 decode action producing each physical result
// register and canonicalizes an identity F32 scale to an unscaled plan.
void loom_amdgpu_select_vector_fp8_decode_plan(
    loom_low_lower_context_t* context,
    loom_amdgpu_vector_16bit_float_conversion_plan_t* plan);

// Returns the stable compile-report strategy key for an FP8/BF8 vector
// conversion plan.
iree_string_view_t loom_amdgpu_vector_fp8_conversion_plan_key(
    const loom_amdgpu_vector_16bit_float_conversion_plan_t* plan);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TARGET_ARCH_AMDGPU_LOWER_ENCODING_FP8_VECTOR_PLAN_H_
