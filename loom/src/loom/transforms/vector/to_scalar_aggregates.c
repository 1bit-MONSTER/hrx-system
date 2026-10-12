// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/transforms/vector/to_scalar_aggregates.h"

#include "loom/ir/module.h"
#include "loom/ops/scf/ops.h"
#include "loom/ops/vector/ops.h"

// Builds all results of one scalarized lane. Single-result operations retain
// the specialized lane lowerers; multi-result descriptors are mechanical and
// materialize their shared operands once.
static iree_status_t loom_vector_to_scalar_build_lanes(
    loom_vector_to_scalar_state_t* state,
    loom_vector_to_scalar_index_list_t indices, uint16_t result_count,
    loom_value_id_t* out_lanes) {
  if (result_count == 1) {
    return loom_vector_to_scalar_build_lane(state, indices, out_lanes);
  }
  return loom_vector_to_scalar_build_generic_lanes(state, indices, result_count,
                                                   out_lanes);
}

static loom_type_t loom_vector_to_scalar_result_type(
    const loom_vector_to_scalar_state_t* state, uint16_t result_ordinal) {
  return loom_module_value_type(
      state->rewriter->module,
      loom_op_const_results(state->op)[result_ordinal]);
}

//===----------------------------------------------------------------------===//
// Static aggregate lowering
//===----------------------------------------------------------------------===//

static iree_status_t loom_vector_to_scalar_lower_static_aggregates(
    loom_vector_to_scalar_state_t* state, uint16_t result_count,
    loom_value_id_t* out_replacements) {
  uint16_t element_count = 0;
  IREE_RETURN_IF_ERROR(loom_vector_to_scalar_static_element_count(
      state, state->vector_type, &element_count));
  if (loom_pass_has_error_diagnostics(state->pass)) {
    return iree_ok_status();
  }
  const iree_host_size_t result_element_count =
      (iree_host_size_t)result_count * element_count;
  loom_value_id_t* elements = NULL;
  if (result_element_count > 0) {
    IREE_RETURN_IF_ERROR(
        iree_arena_allocate_array(state->rewriter->arena, result_element_count,
                                  sizeof(*elements), (void**)&elements));
  }
  loom_value_id_t single_lane = LOOM_VALUE_ID_INVALID;
  loom_value_id_t* lanes = &single_lane;
  if (result_count > 1) {
    IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
        state->rewriter->arena, result_count, sizeof(*lanes), (void**)&lanes));
  }
  const uint8_t rank = loom_type_rank(state->vector_type);
  int64_t* indices = NULL;
  if (rank > 0) {
    IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
        state->rewriter->arena, rank, sizeof(*indices), (void**)&indices));
  }
  for (uint16_t ordinal = 0; ordinal < element_count; ++ordinal) {
    loom_vector_to_scalar_indices_from_ordinal(state->vector_type,
                                               (int64_t)ordinal, indices);
    const loom_vector_to_scalar_index_list_t index_list = {
        .static_indices = indices,
        .rank = rank,
    };
    IREE_RETURN_IF_ERROR(loom_vector_to_scalar_build_lanes(
        state, index_list, result_count, lanes));
    if (loom_pass_has_error_diagnostics(state->pass)) {
      return iree_ok_status();
    }
    for (uint16_t result_ordinal = 0; result_ordinal < result_count;
         ++result_ordinal) {
      elements[(iree_host_size_t)result_ordinal * element_count + ordinal] =
          lanes[result_ordinal];
    }
  }

  for (uint16_t result_ordinal = 0; result_ordinal < result_count;
       ++result_ordinal) {
    const loom_value_id_t* result_elements =
        element_count > 0
            ? &elements[(iree_host_size_t)result_ordinal * element_count]
            : NULL;
    loom_op_t* from_elements_op = NULL;
    IREE_RETURN_IF_ERROR(loom_vector_from_elements_build(
        &state->rewriter->builder, result_elements, element_count,
        loom_vector_to_scalar_result_type(state, result_ordinal),
        state->location, &from_elements_op));
    out_replacements[result_ordinal] =
        loom_vector_from_elements_result(from_elements_op);
  }
  return iree_ok_status();
}

//===----------------------------------------------------------------------===//
// Dynamic aggregate lowering
//===----------------------------------------------------------------------===//

static iree_status_t loom_vector_to_scalar_aggregate_loop_axis(
    loom_vector_to_scalar_state_t* state, uint16_t result_count, uint8_t axis,
    const loom_value_id_t* current_aggregates, loom_value_id_t* dynamic_indices,
    loom_value_id_t* out_aggregates) {
  loom_value_id_t lower_bound = LOOM_VALUE_ID_INVALID;
  loom_value_id_t step = LOOM_VALUE_ID_INVALID;
  loom_value_id_t upper_bound = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_vector_to_scalar_build_scalar_constant(
      &state->rewriter->builder, loom_type_scalar(LOOM_SCALAR_TYPE_INDEX),
      state->location, 0, &lower_bound));
  IREE_RETURN_IF_ERROR(loom_vector_to_scalar_build_scalar_constant(
      &state->rewriter->builder, loom_type_scalar(LOOM_SCALAR_TYPE_INDEX),
      state->location, 1, &step));
  IREE_RETURN_IF_ERROR(loom_vector_to_scalar_dim_bound(
      state, state->vector_type, axis, &upper_bound));

  loom_op_t* loop = NULL;
  IREE_RETURN_IF_ERROR(loom_scf_for_build(
      &state->rewriter->builder, /*build_flags=*/0, lower_bound, upper_bound,
      step, current_aggregates, result_count, /*result_types=*/NULL, NULL, 0,
      /*pipeline_depth=*/LOOM_VALUE_ID_INVALID, LOOM_VALUE_ID_INVALID,
      /*unroll_policy=*/0, /*unroll_schedule=*/0, state->location, &loop));
  loom_vector_to_scalar_record_loop_created(state);

  loom_builder_ip_t saved = loom_builder_enter_region(
      &state->rewriter->builder, loop, loom_scf_for_body(loop));
  dynamic_indices[axis] = loom_region_entry_arg_id(loom_scf_for_body(loop), 0);
  loom_value_id_t single_aggregate_arg = LOOM_VALUE_ID_INVALID;
  loom_value_id_t single_yielded_aggregate = LOOM_VALUE_ID_INVALID;
  loom_value_id_t* aggregate_args = &single_aggregate_arg;
  loom_value_id_t* yielded_aggregates = &single_yielded_aggregate;
  if (result_count > 1) {
    IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
        state->rewriter->arena, result_count, sizeof(*aggregate_args),
        (void**)&aggregate_args));
    IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
        state->rewriter->arena, result_count, sizeof(*yielded_aggregates),
        (void**)&yielded_aggregates));
  }
  for (uint16_t i = 0; i < result_count; ++i) {
    aggregate_args[i] =
        loom_region_entry_arg_id(loom_scf_for_body(loop), (uint16_t)(i + 1));
  }

  if (axis + 1 == loom_type_rank(state->vector_type)) {
    const loom_vector_to_scalar_index_list_t index_list = {
        .dynamic_indices = dynamic_indices,
        .rank = loom_type_rank(state->vector_type),
    };
    loom_value_id_t single_lane = LOOM_VALUE_ID_INVALID;
    loom_value_id_t* lanes = &single_lane;
    if (result_count > 1) {
      IREE_RETURN_IF_ERROR(
          iree_arena_allocate_array(state->rewriter->arena, result_count,
                                    sizeof(*lanes), (void**)&lanes));
    }
    IREE_RETURN_IF_ERROR(loom_vector_to_scalar_build_lanes(
        state, index_list, result_count, lanes));
    if (loom_pass_has_error_diagnostics(state->pass)) {
      loom_builder_restore(&state->rewriter->builder, saved);
      return iree_ok_status();
    }
    for (uint16_t i = 0; i < result_count; ++i) {
      IREE_RETURN_IF_ERROR(loom_vector_to_scalar_insert_lane(
          state, lanes[i], aggregate_args[i],
          loom_vector_to_scalar_result_type(state, i), index_list,
          &yielded_aggregates[i]));
    }
  } else {
    IREE_RETURN_IF_ERROR(loom_vector_to_scalar_aggregate_loop_axis(
        state, result_count, (uint8_t)(axis + 1), aggregate_args,
        dynamic_indices, yielded_aggregates));
    if (loom_pass_has_error_diagnostics(state->pass)) {
      loom_builder_restore(&state->rewriter->builder, saved);
      return iree_ok_status();
    }
  }
  loom_op_t* yield_op = NULL;
  IREE_RETURN_IF_ERROR(loom_scf_yield_build(&state->rewriter->builder,
                                            yielded_aggregates, result_count,
                                            state->location, &yield_op));
  loom_builder_restore(&state->rewriter->builder, saved);

  const loom_value_slice_t loop_results = loom_scf_for_results(loop);
  for (uint16_t i = 0; i < result_count; ++i) {
    out_aggregates[i] = loop_results.values[i];
  }
  return iree_ok_status();
}

static iree_status_t loom_vector_to_scalar_dynamic_seed(
    loom_vector_to_scalar_state_t* state, loom_value_id_t* out_seed) {
  if (state->descriptor.seed_operand_index != UINT8_MAX &&
      state->descriptor.seed_operand_index < state->op->operand_count) {
    loom_value_id_t seed =
        loom_op_const_operands(state->op)[state->descriptor.seed_operand_index];
    loom_type_t seed_type =
        loom_module_value_type(state->rewriter->module, seed);
    if (loom_type_equal(seed_type, state->vector_type)) {
      *out_seed = seed;
      return iree_ok_status();
    }
  }
  return loom_vector_to_scalar_build_vector_zero(state, state->vector_type,
                                                 out_seed);
}

static iree_status_t loom_vector_to_scalar_lower_dynamic_aggregates(
    loom_vector_to_scalar_state_t* state, uint16_t result_count,
    loom_value_id_t* out_replacements) {
  loom_value_id_t single_seed = LOOM_VALUE_ID_INVALID;
  loom_value_id_t* seeds = &single_seed;
  if (result_count == 1) {
    IREE_RETURN_IF_ERROR(loom_vector_to_scalar_dynamic_seed(state, seeds));
  } else {
    IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
        state->rewriter->arena, result_count, sizeof(*seeds), (void**)&seeds));
    for (uint16_t i = 0; i < result_count; ++i) {
      IREE_RETURN_IF_ERROR(loom_vector_to_scalar_build_vector_zero(
          state, loom_vector_to_scalar_result_type(state, i), &seeds[i]));
    }
  }

  const uint8_t rank = loom_type_rank(state->vector_type);
  loom_value_id_t* dynamic_indices = NULL;
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(state->rewriter->arena, rank,
                                                 sizeof(*dynamic_indices),
                                                 (void**)&dynamic_indices));
  IREE_RETURN_IF_ERROR(loom_vector_to_scalar_aggregate_loop_axis(
      state, result_count, 0, seeds, dynamic_indices, out_replacements));
  if (loom_pass_has_error_diagnostics(state->pass)) {
    return iree_ok_status();
  }
  return iree_ok_status();
}

iree_status_t loom_vector_to_scalar_lower_aggregates(
    loom_vector_to_scalar_state_t* state, uint16_t result_count,
    loom_value_id_t* out_replacements) {
  if (loom_type_is_all_static(state->vector_type)) {
    return loom_vector_to_scalar_lower_static_aggregates(state, result_count,
                                                         out_replacements);
  }
  return loom_vector_to_scalar_lower_dynamic_aggregates(state, result_count,
                                                        out_replacements);
}

iree_status_t loom_vector_to_scalar_lower_aggregate(
    loom_vector_to_scalar_state_t* state, loom_value_id_t* out_replacement) {
  return loom_vector_to_scalar_lower_aggregates(state, 1, out_replacement);
}

//===----------------------------------------------------------------------===//
// Splat lowering
//===----------------------------------------------------------------------===//

static iree_status_t loom_vector_to_scalar_lower_static_splat(
    loom_vector_to_scalar_state_t* state, loom_value_id_t* out_replacement) {
  uint16_t element_count = 0;
  IREE_RETURN_IF_ERROR(loom_vector_to_scalar_static_element_count(
      state, state->vector_type, &element_count));
  if (loom_pass_has_error_diagnostics(state->pass)) {
    return iree_ok_status();
  }
  loom_value_id_t scalar = loom_vector_splat_scalar(state->op);
  loom_value_id_t* elements = NULL;
  if (element_count > 0) {
    IREE_RETURN_IF_ERROR(
        iree_arena_allocate_array(state->rewriter->arena, element_count,
                                  sizeof(loom_value_id_t), (void**)&elements));
  }
  for (uint16_t i = 0; i < element_count; ++i) {
    elements[i] = scalar;
  }
  loom_op_t* from_elements_op = NULL;
  IREE_RETURN_IF_ERROR(loom_vector_from_elements_build(
      &state->rewriter->builder, elements, element_count, state->vector_type,
      state->location, &from_elements_op));
  *out_replacement = loom_vector_from_elements_result(from_elements_op);
  return iree_ok_status();
}

static iree_status_t loom_vector_to_scalar_splat_loop_axis(
    loom_vector_to_scalar_state_t* state, uint8_t axis,
    loom_value_id_t current_aggregate, loom_value_id_t scalar,
    loom_value_id_t* dynamic_indices, loom_value_id_t* out_aggregate) {
  loom_value_id_t lower_bound = LOOM_VALUE_ID_INVALID;
  loom_value_id_t step = LOOM_VALUE_ID_INVALID;
  loom_value_id_t upper_bound = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_vector_to_scalar_build_scalar_constant(
      &state->rewriter->builder, loom_type_scalar(LOOM_SCALAR_TYPE_INDEX),
      state->location, 0, &lower_bound));
  IREE_RETURN_IF_ERROR(loom_vector_to_scalar_build_scalar_constant(
      &state->rewriter->builder, loom_type_scalar(LOOM_SCALAR_TYPE_INDEX),
      state->location, 1, &step));
  IREE_RETURN_IF_ERROR(loom_vector_to_scalar_dim_bound(
      state, state->vector_type, axis, &upper_bound));

  loom_op_t* loop = NULL;
  IREE_RETURN_IF_ERROR(loom_scf_for_build(
      &state->rewriter->builder, /*build_flags=*/0, lower_bound, upper_bound,
      step, &current_aggregate, 1, /*result_types=*/NULL, NULL, 0,
      /*pipeline_depth=*/LOOM_VALUE_ID_INVALID, LOOM_VALUE_ID_INVALID,
      /*unroll_policy=*/0, /*unroll_schedule=*/0, state->location, &loop));
  loom_vector_to_scalar_record_loop_created(state);

  loom_builder_ip_t saved = loom_builder_enter_region(
      &state->rewriter->builder, loop, loom_scf_for_body(loop));
  loom_value_id_t aggregate_arg =
      loom_region_entry_arg_id(loom_scf_for_body(loop), 1);
  loom_value_id_t yielded_aggregate = LOOM_VALUE_ID_INVALID;
  dynamic_indices[axis] = loom_region_entry_arg_id(loom_scf_for_body(loop), 0);
  if (axis + 1 == loom_type_rank(state->vector_type)) {
    loom_vector_to_scalar_index_list_t index_list = {
        .dynamic_indices = dynamic_indices,
        .rank = loom_type_rank(state->vector_type),
    };
    IREE_RETURN_IF_ERROR(loom_vector_to_scalar_insert_lane(
        state, scalar, aggregate_arg, state->vector_type, index_list,
        &yielded_aggregate));
  } else {
    IREE_RETURN_IF_ERROR(loom_vector_to_scalar_splat_loop_axis(
        state, (uint8_t)(axis + 1), aggregate_arg, scalar, dynamic_indices,
        &yielded_aggregate));
  }
  loom_op_t* yield_op = NULL;
  IREE_RETURN_IF_ERROR(loom_scf_yield_build(&state->rewriter->builder,
                                            &yielded_aggregate, 1,
                                            state->location, &yield_op));
  loom_builder_restore(&state->rewriter->builder, saved);

  *out_aggregate = loom_scf_for_results(loop).values[0];
  return iree_ok_status();
}

static iree_status_t loom_vector_to_scalar_lower_dynamic_splat(
    loom_vector_to_scalar_state_t* state, loom_value_id_t* out_replacement) {
  loom_value_id_t seed = LOOM_VALUE_ID_INVALID;
  IREE_RETURN_IF_ERROR(loom_vector_to_scalar_build_vector_zero(
      state, state->vector_type, &seed));

  uint8_t rank = loom_type_rank(state->vector_type);
  loom_value_id_t* dynamic_indices = NULL;
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(state->rewriter->arena, rank,
                                                 sizeof(loom_value_id_t),
                                                 (void**)&dynamic_indices));
  return loom_vector_to_scalar_splat_loop_axis(
      state, 0, seed, loom_vector_splat_scalar(state->op), dynamic_indices,
      out_replacement);
}

iree_status_t loom_vector_to_scalar_lower_splat(
    loom_vector_to_scalar_state_t* state, loom_value_id_t* out_replacement) {
  if (loom_type_is_all_static(state->vector_type)) {
    return loom_vector_to_scalar_lower_static_splat(state, out_replacement);
  }
  return loom_vector_to_scalar_lower_dynamic_splat(state, out_replacement);
}
