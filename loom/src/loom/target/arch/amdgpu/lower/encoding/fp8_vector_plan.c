// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/target/arch/amdgpu/lower/encoding/fp8_vector_plan.h"

#include <stdint.h>

#include "loom/ops/vector/ops.h"
#include "loom/ops/vector/storage.h"
#include "loom/target/arch/amdgpu/lower/constants.h"
#include "loom/target/arch/amdgpu/lower/descriptor_ref.h"
#include "loom/target/arch/amdgpu/lower/encoding/fp8.h"
#include "loom/target/arch/amdgpu/lower/types.h"
#include "loom/target/arch/amdgpu/refs/target_refs.h"

typedef struct loom_amdgpu_vector_fp8_decode_value_flag_cache_t {
  // Per-result-lane FP8 decode simplification flags.
  loom_amdgpu_fp8_decode_value_flags_t
      lane_flags[LOOM_AMDGPU_MAX_PACKED_16BIT_FLOAT_LANES];
} loom_amdgpu_vector_fp8_decode_value_flag_cache_t;

static void loom_amdgpu_vector_fp8_decode_value_flag_cache_initialize(
    loom_low_lower_context_t* context,
    const loom_amdgpu_vector_16bit_float_conversion_plan_t* plan,
    loom_amdgpu_vector_fp8_decode_value_flag_cache_t* out_cache) {
  *out_cache = (loom_amdgpu_vector_fp8_decode_value_flag_cache_t){0};
  IREE_ASSERT_LE(plan->lane_count, IREE_ARRAYSIZE(out_cache->lane_flags));
  const loom_value_fact_table_t* fact_table =
      loom_low_lower_context_fact_table(context);
  if (fact_table == NULL) {
    return;
  }

  const loom_value_facts_t content_facts =
      loom_value_fact_table_lookup(fact_table, plan->content_fact_source);
  loom_value_facts_t all_equal_facts = {0};
  const bool has_all_equal_facts = loom_value_facts_query_all_equal_element(
      &fact_table->context, content_facts, &all_equal_facts);

  loom_value_fact_small_static_lanes_t small_lanes = {0};
  const bool has_small_lanes = loom_value_facts_query_small_static_lanes(
      &fact_table->context, content_facts, &small_lanes);
  for (uint32_t lane_index = 0; lane_index < plan->lane_count; ++lane_index) {
    const uint64_t storage_lane =
        (uint64_t)plan->storage_lane_offset +
        (uint64_t)lane_index * (uint64_t)plan->storage_lane_stride;
    loom_value_facts_t lane_facts = {0};
    if (has_small_lanes && storage_lane < small_lanes.count) {
      lane_facts = small_lanes.lanes[storage_lane];
    } else if (has_all_equal_facts) {
      lane_facts = all_equal_facts;
    } else {
      continue;
    }
    out_cache->lane_flags[lane_index] =
        loom_amdgpu_fp8_decode_value_flags_from_facts(lane_facts);
  }
}

static loom_amdgpu_fp8_decode_value_flags_t
loom_amdgpu_vector_fp8_decode_value_flags(
    const loom_amdgpu_vector_fp8_decode_value_flag_cache_t* cache,
    uint32_t lane_index) {
  IREE_ASSERT_LT(lane_index, IREE_ARRAYSIZE(cache->lane_flags));
  return cache->lane_flags[lane_index];
}

typedef struct loom_amdgpu_vector_fp8_selection_state_t {
  // Active low-lowering context.
  loom_low_lower_context_t* context;
  // Vector conversion plan being selected.
  loom_amdgpu_vector_16bit_float_conversion_plan_t* plan;
  // Physical result actions not yet selected.
  uint32_t missing_action_count;
  // Whether software decode resources have been initialized.
  bool software_resources_initialized;
  // Per-result-lane FP8 decode simplification flags.
  loom_amdgpu_vector_fp8_decode_value_flag_cache_t value_flag_cache;
  // Target-supported software FP8 decode operations.
  loom_amdgpu_fp8_decode_plan_t decode_plan;
} loom_amdgpu_vector_fp8_selection_state_t;

static void loom_amdgpu_vector_fp8_selection_require_software_resources(
    loom_amdgpu_vector_fp8_selection_state_t* state) {
  if (state->software_resources_initialized) {
    return;
  }
  loom_amdgpu_vector_fp8_decode_value_flag_cache_initialize(
      state->context, state->plan, &state->value_flag_cache);
  loom_amdgpu_initialize_fp8_decode_plan_from_descriptor_set(
      loom_low_lower_context_descriptor_set(state->context),
      state->plan->source_format, state->plan->descriptor_source_format,
      &state->decode_plan);
  state->software_resources_initialized = true;
}

static loom_amdgpu_fp8_decode_value_flags_t
loom_amdgpu_vector_fp8_pair_decode_value_flags(
    const loom_amdgpu_vector_fp8_decode_value_flag_cache_t* value_flag_cache,
    uint32_t lane_index, uint32_t live_lane_count) {
  IREE_ASSERT_GE(live_lane_count, 1u);
  IREE_ASSERT_LE(live_lane_count, 2u);
  loom_amdgpu_fp8_decode_value_flags_t value_flags =
      loom_amdgpu_vector_fp8_decode_value_flags(value_flag_cache, lane_index);
  if (live_lane_count == 2u) {
    value_flags &= loom_amdgpu_vector_fp8_decode_value_flags(value_flag_cache,
                                                             lane_index + 1u);
  }
  return value_flags;
}

static bool loom_amdgpu_vector_fp8_query_uniform_packed_pair_value_flags(
    loom_amdgpu_vector_fp8_selection_state_t* state,
    loom_amdgpu_fp8_decode_value_flags_t* out_value_flags) {
  const loom_amdgpu_vector_16bit_float_conversion_plan_t* plan = state->plan;
  const uint32_t pair_count = plan->result_register_count;
  const uint32_t required_pair_count = (plan->lane_count + 1u) / 2u;
  if (required_pair_count == 0 ||
      required_pair_count > LOOM_AMDGPU_MAX_PACKED_32BIT_REGISTERS ||
      pair_count != required_pair_count ||
      plan->storage_register_count > LOOM_AMDGPU_MAX_PACKED_32BIT_REGISTERS) {
    return false;
  }

  loom_amdgpu_vector_fp8_pair_storage_t
      pair_storage[LOOM_AMDGPU_MAX_PACKED_32BIT_REGISTERS] = {0};
  for (uint32_t register_index = 0; register_index < pair_count;
       ++register_index) {
    const uint32_t lane_base = register_index * 2u;
    if (!loom_amdgpu_vector_fp8_query_storage_pair(
            plan, lane_base, &pair_storage[register_index])) {
      return false;
    }
  }

  loom_amdgpu_vector_fp8_selection_require_software_resources(state);
  for (uint32_t register_index = 0; register_index < pair_count;
       ++register_index) {
    const uint32_t lane_base = register_index * 2u;
    const loom_amdgpu_fp8_decode_value_flags_t pair_value_flags =
        loom_amdgpu_vector_fp8_pair_decode_value_flags(
            &state->value_flag_cache, lane_base,
            pair_storage[register_index].live_lane_count);
    *out_value_flags = register_index == 0
                           ? pair_value_flags
                           : *out_value_flags & pair_value_flags;
  }
  return true;
}

static bool loom_amdgpu_vector_fp8_scalef32_is_identity(
    loom_low_lower_context_t* context,
    const loom_amdgpu_vector_16bit_float_conversion_plan_t* plan) {
  if (!loom_amdgpu_vector_fp8_plan_has_f32_scale(plan)) {
    return false;
  }
  const loom_module_t* module = loom_low_lower_context_module(context);
  const loom_value_fact_table_t* fact_table =
      loom_low_lower_context_fact_table(context);
  uint32_t scale_bit_pattern = 0;
  const loom_type_t scale_type =
      loom_module_value_type(module, plan->scale_source);
  if (loom_amdgpu_type_is_f32(scale_type)) {
    return loom_amdgpu_value_as_f32_bit_pattern(
               module, fact_table, plan->scale_source, &scale_bit_pattern) &&
           scale_bit_pattern == LOOM_AMDGPU_FP8_F32_IDENTITY_SCALE_BITS;
  }
  return loom_vector_static_rank1_lane_count(scale_type, LOOM_SCALAR_TYPE_F32,
                                             1) == 1 &&
         loom_amdgpu_source_lane_as_u32_bits(
             fact_table, module, plan->scale_source, 0, &scale_bit_pattern) &&
         scale_bit_pattern == LOOM_AMDGPU_FP8_F32_IDENTITY_SCALE_BITS;
}

static bool loom_amdgpu_vector_fp8_plan_has_octet_storage(
    const loom_amdgpu_vector_16bit_float_conversion_plan_t* plan) {
  if (plan->lane_count == 0 || (plan->lane_count & 7u) != 0 ||
      plan->storage_lane_stride != 1u ||
      (plan->storage_lane_offset & 3u) != 0) {
    return false;
  }
  const uint64_t last_storage_lane =
      (uint64_t)plan->storage_lane_offset + (uint64_t)plan->lane_count - 1u;
  return last_storage_lane < (uint64_t)plan->storage_lane_count &&
         last_storage_lane / 4u < (uint64_t)plan->storage_register_count;
}

static bool loom_amdgpu_vector_fp8_descriptor_set_has_ref(
    const loom_low_descriptor_set_t* descriptor_set,
    loom_amdgpu_descriptor_ref_t descriptor_ref) {
  return descriptor_ref != LOOM_AMDGPU_DESCRIPTOR_REF_NONE &&
         loom_amdgpu_descriptor_set_has_ref(descriptor_set, descriptor_ref);
}

static bool loom_amdgpu_vector_fp8_has_scalef32_descriptor(
    const loom_low_descriptor_set_t* descriptor_set,
    loom_value_fact_numeric_format_flags_t source_format,
    loom_scalar_type_t result_element_type) {
  loom_amdgpu_descriptor_ref_t descriptor_ref = LOOM_AMDGPU_DESCRIPTOR_REF_NONE;
  return loom_amdgpu_fp8_scalef32_descriptor_ref(
             source_format, result_element_type, &descriptor_ref) &&
         loom_amdgpu_vector_fp8_descriptor_set_has_ref(descriptor_set,
                                                       descriptor_ref);
}

static bool loom_amdgpu_vector_fp8_has_e8m0_pk8_descriptor(
    const loom_low_descriptor_set_t* descriptor_set,
    loom_value_fact_numeric_format_flags_t source_format,
    loom_scalar_type_t result_element_type) {
  loom_amdgpu_descriptor_ref_t descriptor_ref = LOOM_AMDGPU_DESCRIPTOR_REF_NONE;
  return loom_amdgpu_fp8_e8m0_pk8_descriptor_ref(
             source_format, result_element_type, &descriptor_ref) &&
         loom_amdgpu_vector_fp8_descriptor_set_has_ref(descriptor_set,
                                                       descriptor_ref);
}

static bool loom_amdgpu_vector_fp8_native_descriptor_set_refs(
    const loom_low_descriptor_set_t* descriptor_set,
    loom_value_fact_numeric_format_flags_t source_format,
    loom_scalar_type_t result_element_type,
    loom_amdgpu_fp8_native_descriptor_refs_t* out_refs) {
  *out_refs = (loom_amdgpu_fp8_native_descriptor_refs_t){0};
  if (!loom_amdgpu_fp8_native_descriptor_refs(source_format,
                                              result_element_type, out_refs)) {
    return false;
  }
  out_refs->pair = loom_amdgpu_vector_fp8_descriptor_set_has_ref(descriptor_set,
                                                                 out_refs->pair)
                       ? out_refs->pair
                       : LOOM_AMDGPU_DESCRIPTOR_REF_NONE;
  out_refs->lane = loom_amdgpu_vector_fp8_descriptor_set_has_ref(descriptor_set,
                                                                 out_refs->lane)
                       ? out_refs->lane
                       : LOOM_AMDGPU_DESCRIPTOR_REF_NONE;
  return out_refs->pair != LOOM_AMDGPU_DESCRIPTOR_REF_NONE ||
         out_refs->lane != LOOM_AMDGPU_DESCRIPTOR_REF_NONE;
}

typedef enum loom_amdgpu_vector_fp8_packed_bf16_selection_flag_bits_e {
  LOOM_AMDGPU_VECTOR_FP8_PACKED_BF16_SELECTION_FLAG_NONE = 0u,
  LOOM_AMDGPU_VECTOR_FP8_PACKED_BF16_SELECTION_FLAG_REQUIRE_PREFERRED = 1u << 0,
} loom_amdgpu_vector_fp8_packed_bf16_selection_flag_bits_t;
typedef uint32_t loom_amdgpu_vector_fp8_packed_bf16_selection_flags_t;

static void loom_amdgpu_vector_fp8_set_decode_action(
    loom_amdgpu_vector_fp8_selection_state_t* state, uint32_t action_index,
    uint32_t action_span, loom_amdgpu_fp8_decode_action_t action) {
  loom_amdgpu_vector_16bit_float_conversion_plan_t* plan = state->plan;
  const uint32_t action_count =
      loom_amdgpu_vector_fp8_decode_action_count(plan);
  IREE_ASSERT_GE(action_span, 1u);
  IREE_ASSERT_LE(action_index + action_span, action_count);
  IREE_ASSERT_GE(state->missing_action_count, action_span);
  IREE_ASSERT_EQ(plan->strategy.fp8_decode.actions[action_index].kind,
                 LOOM_AMDGPU_FP8_DECODE_ACTION_KIND_NONE);
  plan->strategy.fp8_decode.actions[action_index] = action;
  for (uint32_t i = 1; i < action_span; ++i) {
    IREE_ASSERT_EQ(plan->strategy.fp8_decode.actions[action_index + i].kind,
                   LOOM_AMDGPU_FP8_DECODE_ACTION_KIND_NONE);
    plan->strategy.fp8_decode.actions[action_index + i].kind =
        LOOM_AMDGPU_FP8_DECODE_ACTION_KIND_CONTINUATION;
  }
  state->missing_action_count -= action_span;
}

static loom_amdgpu_fp8_decode_action_t
loom_amdgpu_vector_fp8_full_decode_action(
    loom_amdgpu_vector_fp8_selection_state_t* state,
    loom_amdgpu_fp8_decode_action_kind_t kind, uint32_t lane_base,
    uint32_t lane_count) {
  IREE_ASSERT_GE(lane_count, 1u);
  IREE_ASSERT_LE(lane_count, 2u);
  loom_amdgpu_vector_fp8_selection_require_software_resources(state);
  loom_amdgpu_fp8_decode_action_t action = {
      .kind = kind,
      .value_flags = loom_amdgpu_vector_fp8_decode_value_flags(
          &state->value_flag_cache, lane_base),
  };
  if (lane_count == 2u) {
    const loom_amdgpu_fp8_decode_value_flags_t high_value_flags =
        loom_amdgpu_vector_fp8_decode_value_flags(&state->value_flag_cache,
                                                  lane_base + 1u);
    action.value_flags |=
        (loom_amdgpu_fp8_decode_value_flags_t)(high_value_flags << 4u);
  }
  return action;
}

static bool loom_amdgpu_vector_fp8_select_packed_pair_action(
    loom_amdgpu_vector_fp8_selection_state_t* state,
    loom_scalar_type_t result_element_type, uint32_t lane_index,
    loom_amdgpu_fp8_decode_action_t* out_action) {
  const loom_amdgpu_vector_16bit_float_conversion_plan_t* plan = state->plan;
  loom_amdgpu_vector_fp8_pair_storage_t pair_storage;
  if (!loom_amdgpu_vector_fp8_query_storage_pair(plan, lane_index,
                                                 &pair_storage)) {
    return false;
  }
  loom_amdgpu_vector_fp8_selection_require_software_resources(state);
  const loom_amdgpu_fp8_decode_value_flags_t value_flags =
      loom_amdgpu_vector_fp8_pair_decode_value_flags(
          &state->value_flag_cache, lane_index, pair_storage.live_lane_count);
  switch (result_element_type) {
    case LOOM_SCALAR_TYPE_BF16:
      if (!loom_amdgpu_can_emit_fp8_pair_to_packed_bf16(&state->decode_plan,
                                                        value_flags)) {
        return false;
      }
      *out_action = loom_amdgpu_select_fp8_packed_bf16_decode_action(
          &state->decode_plan, value_flags);
      return true;
    case LOOM_SCALAR_TYPE_F16:
      if (!loom_amdgpu_can_emit_fp8_pair_to_packed_f16_finite(
              &state->decode_plan, value_flags)) {
        return false;
      }
      *out_action = loom_amdgpu_select_fp8_packed_f16_decode_action(
          &state->decode_plan, value_flags);
      return true;
    default:
      IREE_ASSERT_UNREACHABLE("packed FP8 decode result element type");
      IREE_BUILTIN_UNREACHABLE();
  }
}

static bool loom_amdgpu_vector_fp8_select_all_packed_bf16_actions(
    loom_amdgpu_vector_fp8_selection_state_t* state,
    loom_amdgpu_vector_fp8_packed_bf16_selection_flags_t selection_flags) {
  loom_amdgpu_vector_16bit_float_conversion_plan_t* plan = state->plan;
  if (plan->result_element_type == LOOM_SCALAR_TYPE_F32 &&
      (plan->lane_count & 1u) != 0) {
    return false;
  }
  loom_amdgpu_fp8_decode_value_flags_t value_flags =
      LOOM_AMDGPU_FP8_DECODE_VALUE_FLAG_NONE;
  if (!loom_amdgpu_vector_fp8_query_uniform_packed_pair_value_flags(
          state, &value_flags) ||
      !loom_amdgpu_can_emit_fp8_pair_to_packed_bf16(&state->decode_plan,
                                                    value_flags)) {
    return false;
  }
  if (iree_any_bit_set(
          selection_flags,
          LOOM_AMDGPU_VECTOR_FP8_PACKED_BF16_SELECTION_FLAG_REQUIRE_PREFERRED) &&
      !loom_amdgpu_fp8_prefers_packed_bf16_pair_decode(&state->decode_plan,
                                                       value_flags)) {
    return false;
  }
  const loom_amdgpu_fp8_decode_action_t action =
      loom_amdgpu_select_fp8_packed_bf16_decode_action(&state->decode_plan,
                                                       value_flags);
  const uint32_t action_count =
      loom_amdgpu_vector_fp8_decode_action_count(plan);
  const uint32_t action_span =
      plan->result_element_type == LOOM_SCALAR_TYPE_F32 ? 2u : 1u;
  for (uint32_t i = 0; i < action_count; i += action_span) {
    loom_amdgpu_vector_fp8_set_decode_action(state, i, action_span, action);
  }
  return true;
}

static bool loom_amdgpu_vector_fp8_select_all_packed_f16_actions(
    loom_amdgpu_vector_fp8_selection_state_t* state) {
  loom_amdgpu_vector_16bit_float_conversion_plan_t* plan = state->plan;
  loom_amdgpu_fp8_decode_value_flags_t value_flags =
      LOOM_AMDGPU_FP8_DECODE_VALUE_FLAG_NONE;
  if (!loom_amdgpu_vector_fp8_query_uniform_packed_pair_value_flags(
          state, &value_flags) ||
      !loom_amdgpu_can_emit_fp8_pair_to_packed_f16_finite(&state->decode_plan,
                                                          value_flags)) {
    return false;
  }
  const loom_amdgpu_fp8_decode_action_t action =
      loom_amdgpu_select_fp8_packed_f16_decode_action(&state->decode_plan,
                                                      value_flags);
  for (uint32_t i = 0; i < plan->result_register_count; ++i) {
    loom_amdgpu_vector_fp8_set_decode_action(state, i, 1u, action);
  }
  return true;
}

static bool loom_amdgpu_vector_fp8_has_native_bf16_pack(
    const loom_low_descriptor_set_t* descriptor_set) {
  return loom_amdgpu_descriptor_set_has_ref(
      descriptor_set, LOOM_AMDGPU_DESCRIPTOR_REF_V_CVT_PK_BF16_F32);
}

static bool loom_amdgpu_vector_fp8_select_e8m0_pk8_actions(
    loom_amdgpu_vector_fp8_selection_state_t* state,
    loom_scalar_type_t descriptor_result_element_type) {
  loom_amdgpu_vector_16bit_float_conversion_plan_t* plan = state->plan;
  if (plan->scale_materialization_kind !=
      LOOM_AMDGPU_VECTOR_SCALE_MATERIALIZATION_NONE) {
    return false;
  }
  const loom_low_descriptor_set_t* descriptor_set =
      loom_low_lower_context_descriptor_set(state->context);
  if (!loom_amdgpu_vector_fp8_plan_has_octet_storage(plan) ||
      !loom_amdgpu_vector_fp8_has_e8m0_pk8_descriptor(
          descriptor_set, plan->descriptor_source_format,
          descriptor_result_element_type)) {
    return false;
  }
  loom_amdgpu_fp8_decode_action_kind_t kind =
      LOOM_AMDGPU_FP8_DECODE_ACTION_KIND_NONE;
  switch (descriptor_result_element_type) {
    case LOOM_SCALAR_TYPE_F32:
      if (plan->result_element_type == LOOM_SCALAR_TYPE_F32) {
        kind = LOOM_AMDGPU_FP8_DECODE_ACTION_KIND_IDENTITY_E8M0_PK8_F32;
      } else if (plan->result_element_type == LOOM_SCALAR_TYPE_BF16 &&
                 loom_amdgpu_vector_fp8_has_native_bf16_pack(descriptor_set)) {
        kind =
            LOOM_AMDGPU_FP8_DECODE_ACTION_KIND_IDENTITY_E8M0_PK8_F32_BF16_PACK;
      } else {
        kind =
            LOOM_AMDGPU_FP8_DECODE_ACTION_KIND_IDENTITY_E8M0_PK8_F32_LANES_PACK;
      }
      break;
    case LOOM_SCALAR_TYPE_BF16:
      kind = LOOM_AMDGPU_FP8_DECODE_ACTION_KIND_IDENTITY_E8M0_PK8_BF16;
      break;
    case LOOM_SCALAR_TYPE_F16:
      kind = LOOM_AMDGPU_FP8_DECODE_ACTION_KIND_IDENTITY_E8M0_PK8_F16;
      break;
    default:
      IREE_ASSERT_UNREACHABLE("supported FP8 pk8 result element type");
      IREE_BUILTIN_UNREACHABLE();
  }
  const uint32_t action_span =
      plan->result_element_type == LOOM_SCALAR_TYPE_F32 ? 8u : 4u;
  const uint32_t action_count =
      loom_amdgpu_vector_fp8_decode_action_count(plan);
  for (uint32_t i = 0; i < action_count; i += action_span) {
    loom_amdgpu_vector_fp8_set_decode_action(
        state, i, action_span, (loom_amdgpu_fp8_decode_action_t){.kind = kind});
  }
  return true;
}

static loom_amdgpu_fp8_decode_action_kind_t
loom_amdgpu_vector_fp8_native_f32_pair_action_kind(
    const loom_low_descriptor_set_t* descriptor_set,
    loom_scalar_type_t result_element_type) {
  return result_element_type == LOOM_SCALAR_TYPE_BF16 &&
                 loom_amdgpu_vector_fp8_has_native_bf16_pack(descriptor_set)
             ? LOOM_AMDGPU_FP8_DECODE_ACTION_KIND_NATIVE_F32_PAIR_BF16_PACK
             : LOOM_AMDGPU_FP8_DECODE_ACTION_KIND_NATIVE_F32_PAIR;
}

static loom_amdgpu_fp8_decode_action_kind_t
loom_amdgpu_vector_fp8_native_f32_lanes_action_kind(
    const loom_low_descriptor_set_t* descriptor_set,
    loom_scalar_type_t result_element_type) {
  return result_element_type == LOOM_SCALAR_TYPE_BF16 &&
                 loom_amdgpu_vector_fp8_has_native_bf16_pack(descriptor_set)
             ? LOOM_AMDGPU_FP8_DECODE_ACTION_KIND_NATIVE_F32_LANES_BF16_PACK
             : LOOM_AMDGPU_FP8_DECODE_ACTION_KIND_NATIVE_F32_LANES_PACK;
}

static void loom_amdgpu_vector_fp8_select_unscaled_f32_result_actions(
    loom_amdgpu_vector_fp8_selection_state_t* state) {
  loom_low_lower_context_t* context = state->context;
  loom_amdgpu_vector_16bit_float_conversion_plan_t* plan = state->plan;
  const loom_low_descriptor_set_t* descriptor_set =
      loom_low_lower_context_descriptor_set(context);
  if (loom_amdgpu_vector_fp8_select_e8m0_pk8_actions(state,
                                                     LOOM_SCALAR_TYPE_F32)) {
    return;
  }

  loom_amdgpu_fp8_native_descriptor_refs_t native_refs = {0};
  loom_amdgpu_vector_fp8_native_descriptor_set_refs(
      descriptor_set, plan->descriptor_source_format, LOOM_SCALAR_TYPE_F32,
      &native_refs);
  const bool has_native_pair =
      native_refs.pair != LOOM_AMDGPU_DESCRIPTOR_REF_NONE;
  const bool has_native_lane =
      native_refs.lane != LOOM_AMDGPU_DESCRIPTOR_REF_NONE;
  if ((plan->lane_count & 1u) == 0 && !has_native_pair && !has_native_lane &&
      loom_amdgpu_vector_fp8_select_all_packed_bf16_actions(
          state, LOOM_AMDGPU_VECTOR_FP8_PACKED_BF16_SELECTION_FLAG_NONE)) {
    return;
  }

  for (uint32_t lane_index = 0; lane_index < plan->lane_count;) {
    loom_amdgpu_vector_fp8_pair_storage_t pair_storage;
    if (has_native_pair && loom_amdgpu_vector_fp8_query_storage_pair(
                               plan, lane_index, &pair_storage)) {
      loom_amdgpu_vector_fp8_set_decode_action(
          state, lane_index, pair_storage.live_lane_count,
          (loom_amdgpu_fp8_decode_action_t){
              .kind = LOOM_AMDGPU_FP8_DECODE_ACTION_KIND_NATIVE_F32_PAIR,
          });
      lane_index += pair_storage.live_lane_count;
      continue;
    }
    if (has_native_lane) {
      loom_amdgpu_vector_fp8_set_decode_action(
          state, lane_index, 1u,
          (loom_amdgpu_fp8_decode_action_t){
              .kind = LOOM_AMDGPU_FP8_DECODE_ACTION_KIND_NATIVE_F32_LANE,
          });
      ++lane_index;
      continue;
    }
    loom_amdgpu_fp8_decode_action_t packed_action = {0};
    if (lane_index + 1u < plan->lane_count &&
        loom_amdgpu_vector_fp8_select_packed_pair_action(
            state, LOOM_SCALAR_TYPE_BF16, lane_index, &packed_action)) {
      loom_amdgpu_vector_fp8_set_decode_action(state, lane_index, 2u,
                                               packed_action);
      lane_index += 2u;
      continue;
    }
    loom_amdgpu_vector_fp8_set_decode_action(
        state, lane_index, 1u,
        loom_amdgpu_vector_fp8_full_decode_action(
            state, LOOM_AMDGPU_FP8_DECODE_ACTION_KIND_FULL_F32, lane_index,
            1u));
    ++lane_index;
  }
}

static void loom_amdgpu_vector_fp8_select_f32_result_actions(
    loom_amdgpu_vector_fp8_selection_state_t* state) {
  loom_low_lower_context_t* context = state->context;
  loom_amdgpu_vector_16bit_float_conversion_plan_t* plan = state->plan;
  const loom_low_descriptor_set_t* descriptor_set =
      loom_low_lower_context_descriptor_set(context);
  if (!loom_amdgpu_vector_fp8_plan_uses_f32_scale(plan)) {
    loom_amdgpu_vector_fp8_select_unscaled_f32_result_actions(state);
    return;
  }

  if (loom_amdgpu_vector_fp8_has_scalef32_descriptor(
          descriptor_set, plan->descriptor_source_format,
          LOOM_SCALAR_TYPE_F32)) {
    for (uint32_t lane_index = 0; lane_index < plan->lane_count;
         lane_index += 2u) {
      loom_amdgpu_vector_fp8_pair_storage_t pair_storage;
      if (loom_amdgpu_vector_fp8_query_storage_pair(plan, lane_index,
                                                    &pair_storage)) {
        loom_amdgpu_vector_fp8_set_decode_action(
            state, lane_index, pair_storage.live_lane_count,
            (loom_amdgpu_fp8_decode_action_t){
                .kind = LOOM_AMDGPU_FP8_DECODE_ACTION_KIND_SCALEF32_F32_PAIR,
            });
      }
    }
  }
  if (state->missing_action_count == plan->lane_count) {
    loom_amdgpu_vector_fp8_select_unscaled_f32_result_actions(state);
    return;
  }
  for (uint32_t lane_index = 0; lane_index < plan->lane_count; ++lane_index) {
    if (plan->strategy.fp8_decode.actions[lane_index].kind !=
        LOOM_AMDGPU_FP8_DECODE_ACTION_KIND_NONE) {
      continue;
    }
    loom_amdgpu_vector_fp8_set_decode_action(
        state, lane_index, 1u,
        loom_amdgpu_vector_fp8_full_decode_action(
            state, LOOM_AMDGPU_FP8_DECODE_ACTION_KIND_FULL_F32, lane_index,
            1u));
  }
}

static void loom_amdgpu_vector_fp8_select_f32_fallback_packed_actions(
    loom_amdgpu_vector_fp8_selection_state_t* state) {
  loom_low_lower_context_t* context = state->context;
  loom_amdgpu_vector_16bit_float_conversion_plan_t* plan = state->plan;
  const loom_low_descriptor_set_t* descriptor_set =
      loom_low_lower_context_descriptor_set(context);
  if (loom_amdgpu_vector_fp8_select_e8m0_pk8_actions(state,
                                                     LOOM_SCALAR_TYPE_F32)) {
    return;
  }

  loom_amdgpu_fp8_native_descriptor_refs_t native_refs = {0};
  loom_amdgpu_vector_fp8_native_descriptor_set_refs(
      descriptor_set, plan->descriptor_source_format, LOOM_SCALAR_TYPE_F32,
      &native_refs);
  const bool has_native_pair =
      native_refs.pair != LOOM_AMDGPU_DESCRIPTOR_REF_NONE;
  const bool has_native_lane =
      native_refs.lane != LOOM_AMDGPU_DESCRIPTOR_REF_NONE;
  if ((plan->lane_count & 1u) == 0 && !has_native_pair && !has_native_lane &&
      loom_amdgpu_vector_fp8_select_all_packed_bf16_actions(
          state, LOOM_AMDGPU_VECTOR_FP8_PACKED_BF16_SELECTION_FLAG_NONE)) {
    return;
  }

  const loom_amdgpu_fp8_decode_action_kind_t full_kind =
      plan->result_element_type == LOOM_SCALAR_TYPE_BF16
          ? LOOM_AMDGPU_FP8_DECODE_ACTION_KIND_FULL_BF16
          : LOOM_AMDGPU_FP8_DECODE_ACTION_KIND_FULL_F16;
  for (uint32_t register_index = 0;
       register_index < plan->result_register_count; ++register_index) {
    const uint32_t lane_base = register_index * 2u;
    const uint32_t lane_count = lane_base + 1u < plan->lane_count ? 2u : 1u;
    loom_amdgpu_vector_fp8_pair_storage_t pair_storage;
    if (has_native_pair && loom_amdgpu_vector_fp8_query_storage_pair(
                               plan, lane_base, &pair_storage)) {
      loom_amdgpu_vector_fp8_set_decode_action(
          state, register_index, 1u,
          (loom_amdgpu_fp8_decode_action_t){
              .kind = loom_amdgpu_vector_fp8_native_f32_pair_action_kind(
                  descriptor_set, plan->result_element_type),
          });
      continue;
    }
    if (has_native_lane) {
      loom_amdgpu_vector_fp8_set_decode_action(
          state, register_index, 1u,
          (loom_amdgpu_fp8_decode_action_t){
              .kind = loom_amdgpu_vector_fp8_native_f32_lanes_action_kind(
                  descriptor_set, plan->result_element_type),
          });
      continue;
    }
    loom_amdgpu_fp8_decode_action_t packed_action = {0};
    if (lane_count == 2u &&
        loom_amdgpu_vector_fp8_select_packed_pair_action(
            state, LOOM_SCALAR_TYPE_BF16, lane_base, &packed_action)) {
      loom_amdgpu_vector_fp8_set_decode_action(state, register_index, 1u,
                                               packed_action);
      continue;
    }
    loom_amdgpu_vector_fp8_set_decode_action(
        state, register_index, 1u,
        loom_amdgpu_vector_fp8_full_decode_action(state, full_kind, lane_base,
                                                  lane_count));
  }
}

static void loom_amdgpu_vector_fp8_select_scalef32_packed_actions(
    loom_amdgpu_vector_fp8_selection_state_t* state) {
  loom_low_lower_context_t* context = state->context;
  loom_amdgpu_vector_16bit_float_conversion_plan_t* plan = state->plan;
  const loom_low_descriptor_set_t* descriptor_set =
      loom_low_lower_context_descriptor_set(context);
  const loom_amdgpu_fp8_decode_action_kind_t direct_result_kind =
      plan->result_element_type == LOOM_SCALAR_TYPE_BF16
          ? LOOM_AMDGPU_FP8_DECODE_ACTION_KIND_SCALEF32_BF16_PAIR
          : LOOM_AMDGPU_FP8_DECODE_ACTION_KIND_SCALEF32_F16_PAIR;
  if (loom_amdgpu_vector_fp8_has_scalef32_descriptor(
          descriptor_set, plan->descriptor_source_format,
          plan->result_element_type)) {
    for (uint32_t register_index = 0;
         register_index < plan->result_register_count; ++register_index) {
      loom_amdgpu_vector_fp8_pair_storage_t pair_storage;
      if (loom_amdgpu_vector_fp8_query_storage_pair(plan, register_index * 2u,
                                                    &pair_storage)) {
        loom_amdgpu_vector_fp8_set_decode_action(
            state, register_index, 1u,
            (loom_amdgpu_fp8_decode_action_t){.kind = direct_result_kind});
      }
    }
  }

  if (loom_amdgpu_vector_fp8_has_scalef32_descriptor(
          descriptor_set, plan->descriptor_source_format,
          LOOM_SCALAR_TYPE_F32)) {
    const loom_amdgpu_fp8_decode_action_kind_t f32_pair_kind =
        plan->result_element_type == LOOM_SCALAR_TYPE_BF16 &&
                loom_amdgpu_vector_fp8_has_native_bf16_pack(descriptor_set)
            ? LOOM_AMDGPU_FP8_DECODE_ACTION_KIND_SCALEF32_F32_PAIR_BF16_PACK
            : LOOM_AMDGPU_FP8_DECODE_ACTION_KIND_SCALEF32_F32_PAIR;
    for (uint32_t register_index = 0;
         register_index < plan->result_register_count; ++register_index) {
      if (plan->strategy.fp8_decode.actions[register_index].kind !=
          LOOM_AMDGPU_FP8_DECODE_ACTION_KIND_NONE) {
        continue;
      }
      loom_amdgpu_vector_fp8_pair_storage_t pair_storage;
      if (loom_amdgpu_vector_fp8_query_storage_pair(plan, register_index * 2u,
                                                    &pair_storage)) {
        loom_amdgpu_vector_fp8_set_decode_action(
            state, register_index, 1u,
            (loom_amdgpu_fp8_decode_action_t){.kind = f32_pair_kind});
      }
    }
  }

  if (state->missing_action_count == plan->result_register_count) {
    loom_amdgpu_vector_fp8_select_f32_fallback_packed_actions(state);
    return;
  }
  const loom_amdgpu_fp8_decode_action_kind_t full_kind =
      plan->result_element_type == LOOM_SCALAR_TYPE_BF16
          ? LOOM_AMDGPU_FP8_DECODE_ACTION_KIND_FULL_BF16
          : LOOM_AMDGPU_FP8_DECODE_ACTION_KIND_FULL_F16;
  for (uint32_t register_index = 0;
       register_index < plan->result_register_count; ++register_index) {
    if (plan->strategy.fp8_decode.actions[register_index].kind !=
        LOOM_AMDGPU_FP8_DECODE_ACTION_KIND_NONE) {
      continue;
    }
    const uint32_t lane_base = register_index * 2u;
    const uint32_t lane_count = lane_base + 1u < plan->lane_count ? 2u : 1u;
    loom_amdgpu_vector_fp8_set_decode_action(
        state, register_index, 1u,
        loom_amdgpu_vector_fp8_full_decode_action(state, full_kind, lane_base,
                                                  lane_count));
  }
}

static void loom_amdgpu_vector_fp8_select_unscaled_bf16_actions(
    loom_amdgpu_vector_fp8_selection_state_t* state) {
  loom_low_lower_context_t* context = state->context;
  loom_amdgpu_vector_16bit_float_conversion_plan_t* plan = state->plan;
  const loom_low_descriptor_set_t* descriptor_set =
      loom_low_lower_context_descriptor_set(context);
  if (loom_amdgpu_vector_fp8_select_e8m0_pk8_actions(state,
                                                     LOOM_SCALAR_TYPE_BF16)) {
    return;
  }

  if (loom_amdgpu_vector_fp8_has_scalef32_descriptor(
          descriptor_set, plan->descriptor_source_format,
          LOOM_SCALAR_TYPE_BF16)) {
    for (uint32_t register_index = 0;
         register_index < plan->result_register_count; ++register_index) {
      loom_amdgpu_vector_fp8_pair_storage_t pair_storage;
      if (loom_amdgpu_vector_fp8_query_storage_pair(plan, register_index * 2u,
                                                    &pair_storage)) {
        loom_amdgpu_vector_fp8_set_decode_action(
            state, register_index, 1u,
            (loom_amdgpu_fp8_decode_action_t){
                .kind = LOOM_AMDGPU_FP8_DECODE_ACTION_KIND_SCALEF32_BF16_PAIR,
            });
      }
    }
  }

  loom_amdgpu_fp8_native_descriptor_refs_t native_refs = {0};
  loom_amdgpu_vector_fp8_native_descriptor_set_refs(
      descriptor_set, plan->descriptor_source_format, LOOM_SCALAR_TYPE_F32,
      &native_refs);
  const bool has_native_pair =
      native_refs.pair != LOOM_AMDGPU_DESCRIPTOR_REF_NONE;
  const bool has_native_lane =
      native_refs.lane != LOOM_AMDGPU_DESCRIPTOR_REF_NONE;
  if (state->missing_action_count == plan->result_register_count) {
    const loom_amdgpu_vector_fp8_packed_bf16_selection_flags_t flags =
        has_native_pair || has_native_lane
            ? LOOM_AMDGPU_VECTOR_FP8_PACKED_BF16_SELECTION_FLAG_REQUIRE_PREFERRED
            : LOOM_AMDGPU_VECTOR_FP8_PACKED_BF16_SELECTION_FLAG_NONE;
    if (loom_amdgpu_vector_fp8_select_all_packed_bf16_actions(state, flags)) {
      return;
    }
  }

  for (uint32_t register_index = 0;
       register_index < plan->result_register_count; ++register_index) {
    if (plan->strategy.fp8_decode.actions[register_index].kind !=
        LOOM_AMDGPU_FP8_DECODE_ACTION_KIND_NONE) {
      continue;
    }
    const uint32_t lane_base = register_index * 2u;
    const uint32_t lane_count = lane_base + 1u < plan->lane_count ? 2u : 1u;
    loom_amdgpu_vector_fp8_pair_storage_t pair_storage;
    if (has_native_pair && loom_amdgpu_vector_fp8_query_storage_pair(
                               plan, lane_base, &pair_storage)) {
      loom_amdgpu_vector_fp8_set_decode_action(
          state, register_index, 1u,
          (loom_amdgpu_fp8_decode_action_t){
              .kind = loom_amdgpu_vector_fp8_native_f32_pair_action_kind(
                  descriptor_set, LOOM_SCALAR_TYPE_BF16),
          });
      continue;
    }
    if (has_native_lane) {
      loom_amdgpu_vector_fp8_set_decode_action(
          state, register_index, 1u,
          (loom_amdgpu_fp8_decode_action_t){
              .kind = loom_amdgpu_vector_fp8_native_f32_lanes_action_kind(
                  descriptor_set, LOOM_SCALAR_TYPE_BF16),
          });
      continue;
    }
    loom_amdgpu_fp8_decode_action_t packed_action = {0};
    if (loom_amdgpu_vector_fp8_select_packed_pair_action(
            state, LOOM_SCALAR_TYPE_BF16, lane_base, &packed_action)) {
      loom_amdgpu_vector_fp8_set_decode_action(state, register_index, 1u,
                                               packed_action);
      continue;
    }
    loom_amdgpu_vector_fp8_set_decode_action(
        state, register_index, 1u,
        loom_amdgpu_vector_fp8_full_decode_action(
            state, LOOM_AMDGPU_FP8_DECODE_ACTION_KIND_FULL_BF16, lane_base,
            lane_count));
  }
}

static bool loom_amdgpu_vector_fp8_has_native_f16_byte_select_family(
    const loom_low_descriptor_set_t* descriptor_set,
    const loom_amdgpu_fp8_native_descriptor_refs_t* refs) {
  for (uint32_t i = 0; i < IREE_ARRAYSIZE(refs->byte_select); ++i) {
    if (!loom_amdgpu_vector_fp8_descriptor_set_has_ref(descriptor_set,
                                                       refs->byte_select[i])) {
      return false;
    }
  }
  return true;
}

static void loom_amdgpu_vector_fp8_select_unscaled_f16_actions(
    loom_amdgpu_vector_fp8_selection_state_t* state) {
  loom_low_lower_context_t* context = state->context;
  loom_amdgpu_vector_16bit_float_conversion_plan_t* plan = state->plan;
  const loom_low_descriptor_set_t* descriptor_set =
      loom_low_lower_context_descriptor_set(context);
  if (loom_amdgpu_vector_fp8_select_e8m0_pk8_actions(state,
                                                     LOOM_SCALAR_TYPE_F16)) {
    return;
  }

  loom_amdgpu_fp8_native_descriptor_refs_t f16_refs = {0};
  loom_amdgpu_vector_fp8_native_descriptor_set_refs(
      descriptor_set, plan->descriptor_source_format, LOOM_SCALAR_TYPE_F16,
      &f16_refs);
  const bool has_native_pair = f16_refs.pair != LOOM_AMDGPU_DESCRIPTOR_REF_NONE;
  const bool has_byte_select =
      (plan->lane_count & 1u) != 0 &&
      loom_amdgpu_vector_fp8_has_native_f16_byte_select_family(descriptor_set,
                                                               &f16_refs);
  const uint32_t pair_register_count =
      plan->result_register_count - (has_byte_select ? 1u : 0u);
  if (has_native_pair) {
    for (uint32_t register_index = 0; register_index < pair_register_count;
         ++register_index) {
      loom_amdgpu_vector_fp8_pair_storage_t pair_storage;
      if (loom_amdgpu_vector_fp8_query_storage_pair(plan, register_index * 2u,
                                                    &pair_storage)) {
        loom_amdgpu_vector_fp8_set_decode_action(
            state, register_index, 1u,
            (loom_amdgpu_fp8_decode_action_t){
                .kind = LOOM_AMDGPU_FP8_DECODE_ACTION_KIND_NATIVE_F16_PAIR,
            });
      }
    }
  }
  if (has_byte_select) {
    loom_amdgpu_vector_fp8_set_decode_action(
        state, plan->result_register_count - 1u, 1u,
        (loom_amdgpu_fp8_decode_action_t){
            .kind = LOOM_AMDGPU_FP8_DECODE_ACTION_KIND_NATIVE_F16_BYTE_SELECT,
        });
  }
  if (state->missing_action_count == 0) {
    return;
  }

  if (state->missing_action_count == plan->result_register_count &&
      loom_amdgpu_vector_fp8_has_scalef32_descriptor(
          descriptor_set, plan->descriptor_source_format,
          LOOM_SCALAR_TYPE_F16)) {
    for (uint32_t register_index = 0;
         register_index < plan->result_register_count; ++register_index) {
      loom_amdgpu_vector_fp8_pair_storage_t pair_storage;
      if (loom_amdgpu_vector_fp8_query_storage_pair(plan, register_index * 2u,
                                                    &pair_storage)) {
        loom_amdgpu_vector_fp8_set_decode_action(
            state, register_index, 1u,
            (loom_amdgpu_fp8_decode_action_t){
                .kind = LOOM_AMDGPU_FP8_DECODE_ACTION_KIND_SCALEF32_F16_PAIR,
            });
      }
    }
  }

  loom_amdgpu_fp8_native_descriptor_refs_t f32_refs = {0};
  loom_amdgpu_vector_fp8_native_descriptor_set_refs(
      descriptor_set, plan->descriptor_source_format, LOOM_SCALAR_TYPE_F32,
      &f32_refs);
  if (f32_refs.pair != LOOM_AMDGPU_DESCRIPTOR_REF_NONE) {
    for (uint32_t register_index = 0;
         register_index < plan->result_register_count; ++register_index) {
      if (plan->strategy.fp8_decode.actions[register_index].kind !=
          LOOM_AMDGPU_FP8_DECODE_ACTION_KIND_NONE) {
        continue;
      }
      loom_amdgpu_vector_fp8_pair_storage_t pair_storage;
      if (loom_amdgpu_vector_fp8_query_storage_pair(plan, register_index * 2u,
                                                    &pair_storage)) {
        loom_amdgpu_vector_fp8_set_decode_action(
            state, register_index, 1u,
            (loom_amdgpu_fp8_decode_action_t){
                .kind = LOOM_AMDGPU_FP8_DECODE_ACTION_KIND_NATIVE_F32_PAIR,
            });
      }
    }
  }
  if (state->missing_action_count == 0) {
    return;
  }

  if (state->missing_action_count == plan->result_register_count &&
      loom_amdgpu_vector_fp8_select_all_packed_f16_actions(state)) {
    return;
  }
  for (uint32_t register_index = 0;
       register_index < plan->result_register_count; ++register_index) {
    if (plan->strategy.fp8_decode.actions[register_index].kind !=
        LOOM_AMDGPU_FP8_DECODE_ACTION_KIND_NONE) {
      continue;
    }
    loom_amdgpu_fp8_decode_action_t packed_action = {0};
    if (loom_amdgpu_vector_fp8_select_packed_pair_action(
            state, LOOM_SCALAR_TYPE_F16, register_index * 2u, &packed_action)) {
      loom_amdgpu_vector_fp8_set_decode_action(state, register_index, 1u,
                                               packed_action);
    }
  }
  if (state->missing_action_count == 0) {
    return;
  }
  if (state->missing_action_count == plan->result_register_count) {
    loom_amdgpu_vector_fp8_select_f32_fallback_packed_actions(state);
    return;
  }
  for (uint32_t register_index = 0;
       register_index < plan->result_register_count; ++register_index) {
    if (plan->strategy.fp8_decode.actions[register_index].kind !=
        LOOM_AMDGPU_FP8_DECODE_ACTION_KIND_NONE) {
      continue;
    }
    const uint32_t lane_base = register_index * 2u;
    const uint32_t lane_count = lane_base + 1u < plan->lane_count ? 2u : 1u;
    loom_amdgpu_vector_fp8_set_decode_action(
        state, register_index, 1u,
        loom_amdgpu_vector_fp8_full_decode_action(
            state, LOOM_AMDGPU_FP8_DECODE_ACTION_KIND_FULL_F16, lane_base,
            lane_count));
  }
}

void loom_amdgpu_select_vector_fp8_decode_plan(
    loom_low_lower_context_t* context,
    loom_amdgpu_vector_16bit_float_conversion_plan_t* plan) {
  IREE_ASSERT_EQ(plan->strategy_kind,
                 LOOM_AMDGPU_VECTOR_FLOAT_CONVERSION_STRATEGY_STANDARD);
  plan->strategy_kind = LOOM_AMDGPU_VECTOR_FLOAT_CONVERSION_STRATEGY_FP8_DECODE;
  plan->strategy.fp8_decode = (loom_amdgpu_vector_fp8_decode_plan_t){0};
  if (loom_amdgpu_vector_fp8_scalef32_is_identity(context, plan)) {
    plan->scale_source = LOOM_VALUE_ID_INVALID;
    plan->scale_format = LOOM_VALUE_FACT_NUMERIC_FORMAT_NONE;
    plan->scale_group_element_count = 0;
    plan->scale_count = 0;
    plan->scale_register_count = 0;
  }

  loom_amdgpu_vector_fp8_selection_state_t state;
  state.context = context;
  state.plan = plan;
  state.missing_action_count = loom_amdgpu_vector_fp8_decode_action_count(plan);
  state.software_resources_initialized = false;

  if (loom_amdgpu_vector_fp8_plan_has_e8m0_scale(plan)) {
    const bool selected = loom_amdgpu_vector_fp8_select_e8m0_pk8_actions(
        &state, plan->result_element_type);
    if (!selected) {
      plan->scale_materialization_kind =
          LOOM_AMDGPU_VECTOR_SCALE_MATERIALIZATION_E8M0_F32;
      if (plan->result_element_type == LOOM_SCALAR_TYPE_F32) {
        loom_amdgpu_vector_fp8_select_f32_result_actions(&state);
      } else {
        loom_amdgpu_vector_fp8_select_scalef32_packed_actions(&state);
      }
    }
  } else if (loom_amdgpu_vector_fp8_plan_has_f32_scale(plan)) {
    if (plan->result_element_type == LOOM_SCALAR_TYPE_F32) {
      loom_amdgpu_vector_fp8_select_f32_result_actions(&state);
    } else {
      loom_amdgpu_vector_fp8_select_scalef32_packed_actions(&state);
    }
  } else {
    switch (plan->result_element_type) {
      case LOOM_SCALAR_TYPE_F32:
        loom_amdgpu_vector_fp8_select_f32_result_actions(&state);
        break;
      case LOOM_SCALAR_TYPE_BF16:
        loom_amdgpu_vector_fp8_select_unscaled_bf16_actions(&state);
        break;
      case LOOM_SCALAR_TYPE_F16:
        loom_amdgpu_vector_fp8_select_unscaled_f16_actions(&state);
        break;
      default:
        IREE_ASSERT_UNREACHABLE("supported FP8 vector decode result type");
        IREE_BUILTIN_UNREACHABLE();
    }
  }
  IREE_ASSERT_EQ(state.missing_action_count, 0u);
}

static iree_string_view_t loom_amdgpu_vector_fp8_mixed_conversion_plan_key(
    const loom_amdgpu_vector_16bit_float_conversion_plan_t* plan) {
  const bool scaled = loom_amdgpu_vector_fp8_plan_has_f32_scale(plan);
  switch (plan->result_element_type) {
    case LOOM_SCALAR_TYPE_F32:
      return scaled ? IREE_SV(
                          "amdgpu.vector_16bit_float_conversion.strategy."
                          "fp8_scalef32_mixed_f32_decode")
                    : IREE_SV(
                          "amdgpu.vector_16bit_float_conversion.strategy."
                          "fp8_mixed_f32_decode");
    case LOOM_SCALAR_TYPE_BF16:
      return scaled ? IREE_SV(
                          "amdgpu.vector_16bit_float_conversion.strategy."
                          "fp8_scalef32_mixed_packed_bf16_decode")
                    : IREE_SV(
                          "amdgpu.vector_16bit_float_conversion.strategy."
                          "fp8_mixed_packed_bf16_decode");
    case LOOM_SCALAR_TYPE_F16:
      return scaled ? IREE_SV(
                          "amdgpu.vector_16bit_float_conversion.strategy."
                          "fp8_scalef32_mixed_packed_f16_decode")
                    : IREE_SV(
                          "amdgpu.vector_16bit_float_conversion.strategy."
                          "fp8_mixed_packed_f16_decode");
    default:
      IREE_ASSERT_UNREACHABLE("selected FP8 vector decode result type");
      IREE_BUILTIN_UNREACHABLE();
  }
}

static iree_string_view_t loom_amdgpu_vector_fp8_decode_action_plan_key(
    const loom_amdgpu_vector_16bit_float_conversion_plan_t* plan,
    const loom_amdgpu_fp8_decode_action_t* action) {
  const bool scaled = loom_amdgpu_vector_fp8_plan_has_f32_scale(plan);
  switch (action->kind) {
    case LOOM_AMDGPU_FP8_DECODE_ACTION_KIND_IDENTITY_E8M0_PK8_F32:
      return scaled ? IREE_SV(
                          "amdgpu.vector_16bit_float_conversion.strategy."
                          "fp8_scalef32_e8m0_pk8_f32")
                    : IREE_SV(
                          "amdgpu.vector_16bit_float_conversion.strategy."
                          "fp8_e8m0_pk8_f32");
    case LOOM_AMDGPU_FP8_DECODE_ACTION_KIND_IDENTITY_E8M0_PK8_F32_LANES_PACK:
      if (plan->result_element_type == LOOM_SCALAR_TYPE_BF16) {
        return scaled ? IREE_SV(
                            "amdgpu.vector_16bit_float_conversion.strategy."
                            "fp8_scalef32_e8m0_pk8_f32_manual_bf16_pack")
                      : IREE_SV(
                            "amdgpu.vector_16bit_float_conversion.strategy."
                            "fp8_e8m0_pk8_f32_manual_bf16_pack");
      }
      return scaled ? IREE_SV(
                          "amdgpu.vector_16bit_float_conversion.strategy."
                          "fp8_scalef32_e8m0_pk8_f32_manual_f16_pack")
                    : IREE_SV(
                          "amdgpu.vector_16bit_float_conversion.strategy."
                          "fp8_e8m0_pk8_f32_manual_f16_pack");
    case LOOM_AMDGPU_FP8_DECODE_ACTION_KIND_IDENTITY_E8M0_PK8_F32_BF16_PACK:
      return scaled ? IREE_SV(
                          "amdgpu.vector_16bit_float_conversion.strategy."
                          "fp8_scalef32_e8m0_pk8_f32_native_bf16_pack")
                    : IREE_SV(
                          "amdgpu.vector_16bit_float_conversion.strategy."
                          "fp8_e8m0_pk8_f32_native_bf16_pack");
    case LOOM_AMDGPU_FP8_DECODE_ACTION_KIND_IDENTITY_E8M0_PK8_BF16:
      return IREE_SV(
          "amdgpu.vector_16bit_float_conversion.strategy.fp8_e8m0_pk8_bf16");
    case LOOM_AMDGPU_FP8_DECODE_ACTION_KIND_IDENTITY_E8M0_PK8_F16:
      return IREE_SV(
          "amdgpu.vector_16bit_float_conversion.strategy.fp8_e8m0_pk8_f16");
    case LOOM_AMDGPU_FP8_DECODE_ACTION_KIND_SCALEF32_BF16_PAIR:
      return scaled ? IREE_SV(
                          "amdgpu.vector_16bit_float_conversion.strategy."
                          "fp8_scalef32_native_bf16_pair")
                    : IREE_SV(
                          "amdgpu.vector_16bit_float_conversion.strategy."
                          "fp8_native_bf16_pair");
    case LOOM_AMDGPU_FP8_DECODE_ACTION_KIND_SCALEF32_F16_PAIR:
      return scaled ? IREE_SV(
                          "amdgpu.vector_16bit_float_conversion.strategy."
                          "fp8_scalef32_native_f16_pair")
                    : IREE_SV(
                          "amdgpu.vector_16bit_float_conversion.strategy."
                          "fp8_native_f16_pair");
    case LOOM_AMDGPU_FP8_DECODE_ACTION_KIND_SCALEF32_F32_PAIR:
      if (plan->result_element_type == LOOM_SCALAR_TYPE_F32) {
        return IREE_SV(
            "amdgpu.vector_16bit_float_conversion.strategy."
            "fp8_scalef32_native_f32_pair");
      }
      return plan->result_element_type == LOOM_SCALAR_TYPE_BF16
                 ? IREE_SV(
                       "amdgpu.vector_16bit_float_conversion.strategy."
                       "fp8_scalef32_native_f32_pair_manual_bf16_pack")
                 : IREE_SV(
                       "amdgpu.vector_16bit_float_conversion.strategy."
                       "fp8_scalef32_native_f32_pair_manual_f16_pack");
    case LOOM_AMDGPU_FP8_DECODE_ACTION_KIND_SCALEF32_F32_PAIR_BF16_PACK:
      return IREE_SV(
          "amdgpu.vector_16bit_float_conversion.strategy."
          "fp8_scalef32_native_f32_pair_native_bf16_pack");
    case LOOM_AMDGPU_FP8_DECODE_ACTION_KIND_NATIVE_F16_PAIR:
      return IREE_SV(
          "amdgpu.vector_16bit_float_conversion.strategy.fp8_native_f16_pair");
    case LOOM_AMDGPU_FP8_DECODE_ACTION_KIND_NATIVE_F16_BYTE_SELECT:
      return IREE_SV(
          "amdgpu.vector_16bit_float_conversion.strategy."
          "fp8_native_f16_byte_select");
    case LOOM_AMDGPU_FP8_DECODE_ACTION_KIND_NATIVE_F32_PAIR:
      if (plan->result_element_type == LOOM_SCALAR_TYPE_F32) {
        return scaled ? IREE_SV(
                            "amdgpu.vector_16bit_float_conversion.strategy."
                            "fp8_scalef32_native_f32_pair")
                      : IREE_SV(
                            "amdgpu.vector_16bit_float_conversion.strategy."
                            "fp8_native_f32_pair");
      }
      if (plan->result_element_type == LOOM_SCALAR_TYPE_BF16) {
        return scaled ? IREE_SV(
                            "amdgpu.vector_16bit_float_conversion.strategy."
                            "fp8_scalef32_native_f32_pair_manual_bf16_pack")
                      : IREE_SV(
                            "amdgpu.vector_16bit_float_conversion.strategy."
                            "fp8_native_f32_pair_manual_bf16_pack");
      }
      return scaled ? IREE_SV(
                          "amdgpu.vector_16bit_float_conversion.strategy."
                          "fp8_scalef32_native_f32_pair_manual_f16_pack")
                    : IREE_SV(
                          "amdgpu.vector_16bit_float_conversion.strategy."
                          "fp8_native_f32_pair_manual_f16_pack");
    case LOOM_AMDGPU_FP8_DECODE_ACTION_KIND_NATIVE_F32_PAIR_BF16_PACK:
      return scaled ? IREE_SV(
                          "amdgpu.vector_16bit_float_conversion.strategy."
                          "fp8_scalef32_native_f32_pair_native_bf16_pack")
                    : IREE_SV(
                          "amdgpu.vector_16bit_float_conversion.strategy."
                          "fp8_native_f32_pair_native_bf16_pack");
    case LOOM_AMDGPU_FP8_DECODE_ACTION_KIND_NATIVE_F32_LANE:
      return scaled ? IREE_SV(
                          "amdgpu.vector_16bit_float_conversion.strategy."
                          "fp8_scalef32_native_f32_lane")
                    : IREE_SV(
                          "amdgpu.vector_16bit_float_conversion.strategy."
                          "fp8_native_f32_lane");
    case LOOM_AMDGPU_FP8_DECODE_ACTION_KIND_NATIVE_F32_LANES_PACK:
      if (plan->result_element_type == LOOM_SCALAR_TYPE_BF16) {
        return scaled ? IREE_SV(
                            "amdgpu.vector_16bit_float_conversion.strategy."
                            "fp8_scalef32_native_f32_lane_manual_bf16_pack")
                      : IREE_SV(
                            "amdgpu.vector_16bit_float_conversion.strategy."
                            "fp8_native_f32_lane_manual_bf16_pack");
      }
      return scaled ? IREE_SV(
                          "amdgpu.vector_16bit_float_conversion.strategy."
                          "fp8_scalef32_native_f32_lane_manual_f16_pack")
                    : IREE_SV(
                          "amdgpu.vector_16bit_float_conversion.strategy."
                          "fp8_native_f32_lane_manual_f16_pack");
    case LOOM_AMDGPU_FP8_DECODE_ACTION_KIND_NATIVE_F32_LANES_BF16_PACK:
      return scaled ? IREE_SV(
                          "amdgpu.vector_16bit_float_conversion.strategy."
                          "fp8_scalef32_native_f32_lane_native_bf16_pack")
                    : IREE_SV(
                          "amdgpu.vector_16bit_float_conversion.strategy."
                          "fp8_native_f32_lane_native_bf16_pack");
    case LOOM_AMDGPU_FP8_DECODE_ACTION_KIND_PACKED_BF16_NORMAL:
    case LOOM_AMDGPU_FP8_DECODE_ACTION_KIND_PACKED_BF16_EXACT_REPAIR:
    case LOOM_AMDGPU_FP8_DECODE_ACTION_KIND_PACKED_BF16_EXACT_VIA_F16:
      return scaled ? IREE_SV(
                          "amdgpu.vector_16bit_float_conversion.strategy."
                          "fp8_scalef32_software_packed_bf16_decode")
                    : loom_amdgpu_fp8_packed_bf16_strategy_key(
                          loom_amdgpu_fp8_decode_action_packed_bf16_strategy(
                              action),
                          loom_amdgpu_fp8_decode_action_repairs(action));
    case LOOM_AMDGPU_FP8_DECODE_ACTION_KIND_PACKED_F16_NORMAL:
    case LOOM_AMDGPU_FP8_DECODE_ACTION_KIND_PACKED_F16_EXACT_REPAIR:
      return scaled ? IREE_SV(
                          "amdgpu.vector_16bit_float_conversion.strategy."
                          "fp8_scalef32_software_packed_f16_decode")
                    : loom_amdgpu_fp8_packed_f16_repair_reason_key(
                          loom_amdgpu_fp8_decode_action_repairs(action));
    case LOOM_AMDGPU_FP8_DECODE_ACTION_KIND_FULL_F32:
      return scaled ? IREE_SV(
                          "amdgpu.vector_16bit_float_conversion.strategy."
                          "fp8_scalef32_software_f32_decode")
                    : IREE_SV(
                          "amdgpu.vector_16bit_float_conversion.strategy."
                          "fp8_software_f32_decode");
    case LOOM_AMDGPU_FP8_DECODE_ACTION_KIND_FULL_BF16:
      return scaled ? IREE_SV(
                          "amdgpu.vector_16bit_float_conversion.strategy."
                          "fp8_scalef32_software_packed_bf16_decode")
                    : IREE_SV(
                          "amdgpu.vector_16bit_float_conversion.strategy."
                          "fp8_software_packed_bf16_decode");
    case LOOM_AMDGPU_FP8_DECODE_ACTION_KIND_FULL_F16:
      return scaled ? IREE_SV(
                          "amdgpu.vector_16bit_float_conversion.strategy."
                          "fp8_scalef32_software_packed_f16_decode")
                    : IREE_SV(
                          "amdgpu.vector_16bit_float_conversion.strategy."
                          "fp8_software_packed_f16_decode");
    case LOOM_AMDGPU_FP8_DECODE_ACTION_KIND_NONE:
    case LOOM_AMDGPU_FP8_DECODE_ACTION_KIND_CONTINUATION:
    default:
      IREE_ASSERT_UNREACHABLE("selected FP8 vector decode action");
      IREE_BUILTIN_UNREACHABLE();
  }
}

iree_string_view_t loom_amdgpu_vector_fp8_conversion_plan_key(
    const loom_amdgpu_vector_16bit_float_conversion_plan_t* plan) {
  IREE_ASSERT_EQ(plan->strategy_kind,
                 LOOM_AMDGPU_VECTOR_FLOAT_CONVERSION_STRATEGY_FP8_DECODE);
  if (loom_amdgpu_vector_fp8_plan_materializes_e8m0_f32_scale(plan)) {
    switch (plan->result_element_type) {
      case LOOM_SCALAR_TYPE_F32:
        return IREE_SV(
            "amdgpu.vector_16bit_float_conversion.strategy."
            "fp8_scalee8m0_materialized_f32_decode");
      case LOOM_SCALAR_TYPE_BF16:
        return IREE_SV(
            "amdgpu.vector_16bit_float_conversion.strategy."
            "fp8_scalee8m0_materialized_packed_bf16_decode");
      case LOOM_SCALAR_TYPE_F16:
        return IREE_SV(
            "amdgpu.vector_16bit_float_conversion.strategy."
            "fp8_scalee8m0_materialized_packed_f16_decode");
      default:
        IREE_ASSERT_UNREACHABLE("selected FP8 vector decode result type");
        IREE_BUILTIN_UNREACHABLE();
    }
  }
  const uint32_t action_count =
      loom_amdgpu_vector_fp8_decode_action_count(plan);
  iree_string_view_t plan_key = iree_string_view_empty();
  for (uint32_t action_index = 0; action_index < action_count; ++action_index) {
    const loom_amdgpu_fp8_decode_action_t* action =
        &plan->strategy.fp8_decode.actions[action_index];
    if (action->kind == LOOM_AMDGPU_FP8_DECODE_ACTION_KIND_CONTINUATION) {
      continue;
    }
    const iree_string_view_t action_key =
        loom_amdgpu_vector_fp8_decode_action_plan_key(plan, action);
    if (iree_string_view_is_empty(plan_key)) {
      plan_key = action_key;
    } else if (!iree_string_view_equal(plan_key, action_key)) {
      return loom_amdgpu_vector_fp8_mixed_conversion_plan_key(plan);
    }
  }
  return plan_key;
}
