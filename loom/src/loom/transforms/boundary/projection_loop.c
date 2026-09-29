// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/transforms/boundary/projection_loop.h"

#include <string.h>

#include "loom/ir/context.h"
#include "loom/ir/module.h"
#include "loom/rewrite/loop_like.h"
#include "loom/rewrite/remap.h"

static bool loom_boundary_projection_loop_state_has_candidates(
    const loom_boundary_projection_loop_state_t* state) {
  return state->result_candidate != IREE_HOST_SIZE_MAX;
}

static void loom_boundary_projection_reject_loop_state(
    loom_boundary_projection_function_t* function,
    const loom_boundary_projection_loop_state_t* state) {
  if (state->result_candidate != IREE_HOST_SIZE_MAX) {
    function->candidates[state->result_candidate].selected = false;
  }
  if (state->condition_candidate != IREE_HOST_SIZE_MAX) {
    function->candidates[state->condition_candidate].selected = false;
  }
  if (state->body_candidate != IREE_HOST_SIZE_MAX) {
    function->candidates[state->body_candidate].selected = false;
  }
}

static bool loom_boundary_projection_loop_state_is_selected(
    const loom_boundary_projection_function_t* function,
    const loom_boundary_projection_loop_state_t* state) {
  if (!loom_boundary_projection_loop_state_has_candidates(state)) {
    return false;
  }
  const bool result_selected =
      function->candidates[state->result_candidate].selected;
  const bool condition_selected =
      state->condition_candidate == IREE_HOST_SIZE_MAX ||
      function->candidates[state->condition_candidate].selected;
  const bool body_selected =
      function->candidates[state->body_candidate].selected;
  return result_selected && condition_selected && body_selected;
}

static iree_status_t loom_boundary_projection_add_loop_endpoint(
    loom_boundary_projection_plan_t* plan,
    loom_boundary_projection_function_t* function, loom_value_id_t value_id,
    loom_block_t* block) {
  return loom_boundary_projection_add_provisional_slot(
      plan, function, value_id, LOOM_BOUNDARY_PROJECTION_SLOT_LOOP_STATE,
      block);
}

iree_status_t loom_boundary_projection_collect_loop(
    loom_boundary_projection_plan_t* plan,
    loom_boundary_projection_function_t* function, loom_loop_like_t loop) {
  ++plan->loops_checked;
  const loom_value_slice_t initial_values = loom_loop_like_iter_args(loop);
  if (initial_values.count == 0) {
    return iree_ok_status();
  }
  IREE_ASSERT_EQ(loop.op->result_count, initial_values.count);

  bool* claimable = NULL;
  IREE_RETURN_IF_ERROR(
      iree_arena_allocate_array(plan->arena, initial_values.count,
                                sizeof(*claimable), (void**)&claimable));
  bool any_claimable = false;
  for (uint16_t i = 0; i < initial_values.count; ++i) {
    claimable[i] = loom_boundary_projection_may_claim_slot(
        plan, function, LOOM_BOUNDARY_PROJECTION_SLOT_LOOP_STATE,
        loom_op_const_results(loop.op)[i], /*block=*/NULL);
    any_claimable |= claimable[i];
  }
  if (!any_claimable) {
    return iree_ok_status();
  }

  if (function->loop_count == function->loop_capacity) {
    IREE_RETURN_IF_ERROR(iree_arena_grow_array(
        plan->arena, function->loop_count, function->loop_count + 1,
        sizeof(*function->loops), &function->loop_capacity,
        (void**)&function->loops));
  }
  loom_boundary_projection_loop_t* loop_plan =
      &function->loops[function->loop_count++];
  *loop_plan = (loom_boundary_projection_loop_t){
      .loop = loop,
      .state_count = initial_values.count,
  };
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      plan->arena, loop_plan->state_count, sizeof(*loop_plan->states),
      (void**)&loop_plan->states));

  loom_region_t* body_region = loom_loop_like_body(loop);
  loom_block_t* body_entry = loom_region_entry_block(body_region);
  loop_plan->body_terminator = body_entry->last_op;
  const uint16_t body_state_offset =
      loom_loop_like_iv(loop) == LOOM_VALUE_ID_INVALID ? 0 : 1;
  IREE_ASSERT_EQ(body_entry->arg_count,
                 (uint32_t)body_state_offset + loop_plan->state_count);

  loom_block_t* condition_entry = NULL;
  loom_region_t* condition_region = loom_loop_like_condition_region(loop);
  if (condition_region) {
    condition_entry = loom_region_entry_block(condition_region);
    loop_plan->condition_terminator = condition_entry->last_op;
    IREE_ASSERT_EQ(condition_entry->arg_count, loop_plan->state_count);
  }

  for (uint16_t i = 0; i < loop_plan->state_count; ++i) {
    loom_boundary_projection_loop_state_t* state = &loop_plan->states[i];
    *state = (loom_boundary_projection_loop_state_t){
        .result_value_id = loom_op_const_results(loop.op)[i],
        .condition_value_id = condition_entry
                                  ? loom_block_arg_id(condition_entry, i)
                                  : LOOM_VALUE_ID_INVALID,
        .body_value_id =
            loom_block_arg_id(body_entry, (uint16_t)(body_state_offset + i)),
        .result_candidate = IREE_HOST_SIZE_MAX,
        .condition_candidate = IREE_HOST_SIZE_MAX,
        .body_candidate = IREE_HOST_SIZE_MAX,
    };
    if (!claimable[i]) {
      continue;
    }
    IREE_RETURN_IF_ERROR(loom_boundary_projection_add_loop_endpoint(
        plan, function, state->result_value_id, /*block=*/NULL));
    if (condition_entry) {
      IREE_RETURN_IF_ERROR(loom_boundary_projection_add_loop_endpoint(
          plan, function, state->condition_value_id, condition_entry));
    }
    IREE_RETURN_IF_ERROR(loom_boundary_projection_add_loop_endpoint(
        plan, function, state->body_value_id, body_entry));
  }
  return iree_ok_status();
}

void loom_boundary_projection_index_loops(
    loom_boundary_projection_function_t* function) {
  for (iree_host_size_t loop_index = 0; loop_index < function->loop_count;
       ++loop_index) {
    loom_boundary_projection_loop_t* loop = &function->loops[loop_index];
    for (uint16_t i = 0; i < loop->state_count; ++i) {
      loom_boundary_projection_loop_state_t* state = &loop->states[i];
      const iree_host_size_t result_candidate =
          loom_boundary_projection_slot_index(function, state->result_value_id);
      if (result_candidate == IREE_HOST_SIZE_MAX) {
        continue;
      }
      state->result_candidate = result_candidate;
      state->body_candidate =
          loom_boundary_projection_slot_index(function, state->body_value_id);
      IREE_ASSERT_NE(state->body_candidate, IREE_HOST_SIZE_MAX);
      if (state->condition_value_id != LOOM_VALUE_ID_INVALID) {
        state->condition_candidate = loom_boundary_projection_slot_index(
            function, state->condition_value_id);
        IREE_ASSERT_NE(state->condition_candidate, IREE_HOST_SIZE_MAX);
      }
    }
  }
}

static iree_status_t loom_boundary_projection_couple_loop_endpoint(
    loom_boundary_projection_plan_t* plan,
    loom_boundary_projection_function_t* function, iree_host_size_t lhs,
    iree_host_size_t rhs) {
  IREE_RETURN_IF_ERROR(loom_boundary_projection_add_dependency(
      plan, function, lhs, rhs, /*orders_realization=*/false));
  return loom_boundary_projection_add_dependency(plan, function, rhs, lhs,
                                                 /*orders_realization=*/false);
}

static bool loom_boundary_projection_loop_defines_endpoint(
    const loom_module_t* module, const loom_boundary_projection_loop_t* loop,
    loom_value_id_t value_id) {
  const loom_value_t* value = loom_module_value(module, value_id);
  if (!loom_value_is_block_arg(value)) {
    return loom_value_def_op(value) == loop->loop.op;
  }
  const loom_block_t* block = loom_value_def_block(value);
  for (uint8_t i = 0; i < loop->loop.op->region_count; ++i) {
    if (block->parent_region == loom_op_regions(loop->loop.op)[i]) {
      return true;
    }
  }
  return false;
}

static bool loom_boundary_projection_loop_endpoint_is_type_provider(
    const loom_boundary_projection_plan_t* plan,
    const loom_boundary_projection_loop_t* loop, loom_value_id_t value_id) {
  loom_type_use_iterator_t users;
  loom_module_value_type_users(plan->module, value_id, &users);
  for (loom_value_id_t user = loom_type_users_next(&users);
       user != LOOM_VALUE_ID_INVALID; user = loom_type_users_next(&users)) {
    if (loom_boundary_projection_loop_defines_endpoint(plan->module, loop,
                                                       user)) {
      return true;
    }
  }
  return false;
}

static bool loom_boundary_projection_terminator_supports_state_rebuild(
    const loom_module_t* module, const loom_op_t* terminator,
    uint16_t state_operand_offset, uint16_t state_count) {
  if (!terminator ||
      terminator->operand_count != state_operand_offset + state_count ||
      terminator->result_count != 0 || terminator->successor_count != 0 ||
      terminator->region_count != 0 || terminator->tied_result_count != 0) {
    return false;
  }
  const loom_op_vtable_t* vtable = loom_op_vtable(module, terminator);
  const uint8_t segment_count = loom_op_vtable_operand_segment_count(vtable);
  if (segment_count == 0) {
    return true;
  }
  const uint16_t* segment_counts =
      loom_op_const_operand_segment_counts(terminator);
  uint16_t offset = 0;
  for (uint8_t i = 0; i < segment_count; ++i) {
    if (offset == state_operand_offset && segment_counts[i] == state_count) {
      for (uint8_t j = (uint8_t)(i + 1); j < segment_count; ++j) {
        if (segment_counts[j] != 0) {
          return false;
        }
      }
      return true;
    }
    offset = (uint16_t)(offset + segment_counts[i]);
  }
  return false;
}

static void loom_boundary_projection_reject_loop(
    loom_boundary_projection_function_t* function,
    const loom_boundary_projection_loop_t* loop) {
  for (uint16_t i = 0; i < loop->state_count; ++i) {
    loom_boundary_projection_reject_loop_state(function, &loop->states[i]);
  }
}

static iree_status_t loom_boundary_projection_finalize_loop_state_schema(
    loom_boundary_projection_plan_t* plan,
    loom_boundary_projection_function_t* function,
    loom_boundary_projection_loop_t* loop,
    loom_boundary_projection_loop_state_t* state) {
  loom_boundary_projection_schema_t schema = {0};
  bool claimed = false;
  IREE_RETURN_IF_ERROR(loom_boundary_projection_plan_slot_schema(
      plan, function, LOOM_BOUNDARY_PROJECTION_SLOT_LOOP_STATE,
      state->result_value_id, /*block=*/NULL, &schema, &claimed));
  if (!claimed) {
    return iree_ok_status();
  }
  IREE_RETURN_IF_ERROR(loom_boundary_projection_finalize_provisional_slot(
      plan, function, state->result_candidate, &schema));
  IREE_RETURN_IF_ERROR(loom_boundary_projection_finalize_provisional_slot(
      plan, function, state->body_candidate, &schema));
  IREE_RETURN_IF_ERROR(loom_boundary_projection_couple_loop_endpoint(
      plan, function, state->result_candidate, state->body_candidate));
  if (state->condition_candidate != IREE_HOST_SIZE_MAX) {
    IREE_RETURN_IF_ERROR(loom_boundary_projection_finalize_provisional_slot(
        plan, function, state->condition_candidate, &schema));
    IREE_RETURN_IF_ERROR(loom_boundary_projection_couple_loop_endpoint(
        plan, function, state->result_candidate, state->condition_candidate));
  }

  const loom_value_id_t endpoints[] = {
      state->result_value_id,
      state->condition_value_id,
      state->body_value_id,
  };
  // All recurrence endpoint types are established before logical values are
  // reconstructed. Keep a column intact when another endpoint type needs it.
  for (iree_host_size_t i = 0; i < IREE_ARRAYSIZE(endpoints); ++i) {
    if (endpoints[i] != LOOM_VALUE_ID_INVALID &&
        loom_boundary_projection_loop_endpoint_is_type_provider(plan, loop,
                                                                endpoints[i])) {
      loom_boundary_projection_reject_loop_state(function, state);
      break;
    }
  }
  return iree_ok_status();
}

static iree_status_t loom_boundary_projection_plan_loop_source(
    loom_boundary_projection_plan_t* plan,
    loom_boundary_projection_function_t* function,
    loom_boundary_projection_loop_state_t* state,
    iree_host_size_t destination_candidate, loom_value_id_t source_value_id,
    loom_op_t* boundary_op, loom_boundary_projection_source_t* out_source) {
  const loom_boundary_projection_slot_t* destination =
      &function->candidates[destination_candidate];
  bool planned = false;
  IREE_RETURN_IF_ERROR(destination->schema.rule->transport.plan_source(
      destination->schema.rule, plan, function, destination,
      &destination->schema, source_value_id, boundary_op, out_source,
      &planned));
  if (!planned) {
    loom_boundary_projection_reject_loop_state(function, state);
  }
  return iree_ok_status();
}

iree_status_t loom_boundary_projection_plan_loops(
    loom_boundary_projection_plan_t* plan,
    loom_boundary_projection_function_t* function) {
  for (iree_host_size_t loop_index = 0; loop_index < function->loop_count;
       ++loop_index) {
    loom_boundary_projection_loop_t* loop = &function->loops[loop_index];
    for (uint16_t i = 0; i < loop->state_count; ++i) {
      loom_boundary_projection_loop_state_t* state = &loop->states[i];
      if (loom_boundary_projection_loop_state_has_candidates(state)) {
        IREE_RETURN_IF_ERROR(
            loom_boundary_projection_finalize_loop_state_schema(plan, function,
                                                                loop, state));
      }
    }
  }

  for (iree_host_size_t loop_index = 0; loop_index < function->loop_count;
       ++loop_index) {
    loom_boundary_projection_loop_t* loop = &function->loops[loop_index];
    const bool condition_loop = loop->condition_terminator != NULL;
    if (!loom_boundary_projection_terminator_supports_state_rebuild(
            plan->module, loop->body_terminator, /*state_operand_offset=*/0,
            loop->state_count) ||
        (condition_loop &&
         !loom_boundary_projection_terminator_supports_state_rebuild(
             plan->module, loop->condition_terminator,
             /*state_operand_offset=*/1, loop->state_count))) {
      loom_boundary_projection_reject_loop(function, loop);
      continue;
    }

    const loom_value_slice_t initial_values =
        loom_loop_like_iter_args(loop->loop);
    const loom_value_id_t* backedge_values =
        loom_op_const_operands(loop->body_terminator);
    const loom_value_id_t* condition_values =
        condition_loop ? loom_op_const_operands(loop->condition_terminator)
                       : NULL;
    for (uint16_t i = 0; i < loop->state_count; ++i) {
      loom_boundary_projection_loop_state_t* state = &loop->states[i];
      if (!loom_boundary_projection_loop_state_is_selected(function, state)) {
        loom_boundary_projection_reject_loop_state(function, state);
        continue;
      }
      const iree_host_size_t entry_candidate =
          condition_loop ? state->condition_candidate : state->body_candidate;
      IREE_RETURN_IF_ERROR(loom_boundary_projection_plan_loop_source(
          plan, function, state, entry_candidate, initial_values.values[i],
          loop->loop.op, &state->initial_source));
      IREE_RETURN_IF_ERROR(loom_boundary_projection_plan_loop_source(
          plan, function, state, entry_candidate, backedge_values[i],
          loop->body_terminator, &state->backedge_source));
      if (condition_loop) {
        IREE_RETURN_IF_ERROR(loom_boundary_projection_plan_loop_source(
            plan, function, state, state->body_candidate,
            condition_values[1 + i], loop->condition_terminator,
            &state->condition_source));
      }
    }
  }
  return iree_ok_status();
}

static uint16_t loom_boundary_projection_loop_state_physical_count(
    const loom_boundary_projection_function_t* function,
    const loom_boundary_projection_loop_state_t* state) {
  return loom_boundary_projection_loop_state_is_selected(function, state)
             ? function->candidates[state->result_candidate]
                   .schema.component_count
             : 1;
}

static bool loom_boundary_projection_loop_ties_are_supported(
    const loom_boundary_projection_function_t* function,
    const loom_boundary_projection_loop_t* loop) {
  const loom_value_slice_t initial = loom_loop_like_iter_args(loop->loop);
  const uint16_t state_operand_offset =
      (uint16_t)(initial.values - loom_op_const_operands(loop->loop.op));
  const loom_tied_result_t* ties = loom_op_tied_results(loop->loop.op);
  uint32_t final_tie_count = 0;
  for (uint16_t i = 0; i < loop->loop.op->tied_result_count; ++i) {
    const loom_tied_result_t* tie = &ties[i];
    IREE_ASSERT_LT(tie->result_index, loop->state_count);
    const uint16_t result_count =
        loom_boundary_projection_loop_state_physical_count(
            function, &loop->states[tie->result_index]);
    uint16_t operand_count = 1;
    if (tie->operand_index >= state_operand_offset &&
        tie->operand_index < state_operand_offset + loop->state_count) {
      const uint16_t state_index =
          (uint16_t)(tie->operand_index - state_operand_offset);
      operand_count = loom_boundary_projection_loop_state_physical_count(
          function, &loop->states[state_index]);
    }
    if (result_count != operand_count) {
      return false;
    }
    final_tie_count += result_count;
  }
  return final_tie_count <= UINT16_MAX;
}

void loom_boundary_projection_preflight_loops(
    loom_boundary_projection_plan_t* plan,
    loom_boundary_projection_function_t* function) {
  for (iree_host_size_t loop_index = 0; loop_index < function->loop_count;
       ++loop_index) {
    loom_boundary_projection_loop_t* loop = &function->loops[loop_index];
    uint32_t final_state_count = 0;
    for (uint16_t i = 0; i < loop->state_count; ++i) {
      loom_boundary_projection_loop_state_t* state = &loop->states[i];
      if (!loom_boundary_projection_loop_state_is_selected(function, state)) {
        loom_boundary_projection_reject_loop_state(function, state);
        ++final_state_count;
        continue;
      }
      const loom_boundary_projection_slot_t* candidate =
          &function->candidates[state->result_candidate];
      bool references_projected_state = false;
      for (uint16_t component = 0;
           component < candidate->schema.component_count; ++component) {
        const loom_type_t type = candidate->schema.component_types[component];
        for (uint16_t source = 0; source < loop->state_count; ++source) {
          if (!loom_boundary_projection_loop_state_is_selected(
                  function, &loop->states[source])) {
            continue;
          }
          const loom_boundary_projection_loop_state_t* source_state =
              &loop->states[source];
          references_projected_state |= loom_type_references_value(
              plan->module, type, source_state->result_value_id);
          references_projected_state |=
              source_state->condition_value_id != LOOM_VALUE_ID_INVALID &&
              loom_type_references_value(plan->module, type,
                                         source_state->condition_value_id);
          references_projected_state |= loom_type_references_value(
              plan->module, type, source_state->body_value_id);
        }
      }
      if (references_projected_state) {
        loom_boundary_projection_reject_loop_state(function, state);
        ++final_state_count;
      } else {
        final_state_count += candidate->schema.component_count;
      }
    }
    const uint32_t target_operand_count =
        (uint32_t)loop->loop.op->operand_count - loop->state_count +
        final_state_count;
    const uint32_t body_argument_count =
        final_state_count +
        (loom_loop_like_iv(loop->loop) == LOOM_VALUE_ID_INVALID ? 0u : 1u);
    const uint32_t condition_operand_count =
        final_state_count + (loop->condition_terminator ? 1u : 0u);
    if (final_state_count > UINT16_MAX || target_operand_count > UINT16_MAX ||
        body_argument_count > UINT16_MAX ||
        condition_operand_count > UINT16_MAX ||
        !loom_boundary_projection_loop_ties_are_supported(function, loop)) {
      loom_boundary_projection_reject_loop(function, loop);
    }
  }
}

iree_status_t loom_boundary_projection_finalize_loops(
    loom_boundary_projection_plan_t* plan,
    loom_boundary_projection_function_t* function) {
  for (iree_host_size_t loop_index = 0; loop_index < function->loop_count;
       ++loop_index) {
    loom_boundary_projection_loop_t* loop = &function->loops[loop_index];
    IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
        plan->arena, (iree_host_size_t)loop->state_count + 1,
        sizeof(*loop->state_offsets), (void**)&loop->state_offsets));
    uint32_t final_state_count = 0;
    loop->state_offsets[0] = 0;
    loop->selected = false;
    for (uint16_t i = 0; i < loop->state_count; ++i) {
      const loom_boundary_projection_loop_state_t* state = &loop->states[i];
      const bool selected =
          loom_boundary_projection_loop_state_is_selected(function, state);
      loop->selected |= selected;
      final_state_count +=
          loom_boundary_projection_loop_state_physical_count(function, state);
      IREE_ASSERT_LE(final_state_count, UINT16_MAX);
      loop->state_offsets[i + 1] = (uint16_t)final_state_count;
    }
    loop->final_state_count = (uint16_t)final_state_count;
  }
  return iree_ok_status();
}

static iree_status_t loom_boundary_projection_loop_initialize_remap(
    loom_boundary_projection_plan_t* plan, loom_ir_remap_t* out_remap) {
  return loom_ir_remap_initialize(
      plan->module, plan->module, plan->arena,
      &(loom_ir_remap_options_t){
          .allow_unmapped_values = true,
          .remap_symbol = loom_ir_remap_symbol_callback_empty(),
      },
      out_remap);
}

static iree_status_t loom_boundary_projection_materialize_loop_source(
    loom_boundary_projection_plan_t* plan,
    loom_boundary_projection_function_t* function,
    const loom_boundary_projection_slot_t* candidate,
    const loom_boundary_projection_source_t* source,
    loom_value_id_t* out_values) {
  IREE_ASSERT_EQ(source->rule, candidate->schema.rule);
  return candidate->schema.rule->transport.materialize_source(
      candidate->schema.rule, plan, function, source, out_values);
}

static iree_status_t loom_boundary_projection_materialize_initial_state(
    loom_boundary_projection_plan_t* plan,
    loom_boundary_projection_function_t* function,
    const loom_boundary_projection_loop_t* loop,
    loom_value_id_t* out_initial_values) {
  const loom_value_slice_t source_initial =
      loom_loop_like_iter_args(loop->loop);
  loom_builder_set_before(&plan->rewriter.builder, loop->loop.op);
  for (uint16_t i = 0; i < loop->state_count; ++i) {
    const loom_boundary_projection_loop_state_t* state = &loop->states[i];
    const uint16_t offset = loop->state_offsets[i];
    if (!loom_boundary_projection_loop_state_is_selected(function, state)) {
      out_initial_values[offset] = source_initial.values[i];
      continue;
    }
    const loom_boundary_projection_slot_t* candidate =
        &function->candidates[state->result_candidate];
    IREE_RETURN_IF_ERROR(loom_boundary_projection_materialize_loop_source(
        plan, function, candidate, &state->initial_source,
        out_initial_values ? out_initial_values + offset : NULL));
  }
  return iree_ok_status();
}

static iree_status_t loom_boundary_projection_build_loop_result_types(
    loom_boundary_projection_plan_t* plan,
    loom_boundary_projection_function_t* function,
    const loom_boundary_projection_loop_t* loop,
    const loom_value_id_t* reserved_results, loom_type_t* out_result_types) {
  loom_ir_remap_t remap = {0};
  IREE_RETURN_IF_ERROR(
      loom_boundary_projection_loop_initialize_remap(plan, &remap));
  for (uint16_t i = 0; i < loop->state_count; ++i) {
    const loom_boundary_projection_loop_state_t* state = &loop->states[i];
    if (!loom_boundary_projection_loop_state_is_selected(function, state)) {
      IREE_RETURN_IF_ERROR(
          loom_ir_remap_map_value(&remap, state->result_value_id,
                                  reserved_results[loop->state_offsets[i]]));
    }
  }
  for (uint16_t i = 0; i < loop->state_count; ++i) {
    const loom_boundary_projection_loop_state_t* state = &loop->states[i];
    uint16_t target = loop->state_offsets[i];
    if (!loom_boundary_projection_loop_state_is_selected(function, state)) {
      IREE_RETURN_IF_ERROR(loom_ir_remap_type(
          &remap, loom_module_value_type(plan->module, state->result_value_id),
          &out_result_types[target]));
      continue;
    }
    const loom_boundary_projection_schema_t* schema =
        &function->candidates[state->result_candidate].schema;
    for (uint16_t component = 0; component < schema->component_count;
         ++component) {
      IREE_RETURN_IF_ERROR(
          loom_ir_remap_type(&remap, schema->component_types[component],
                             &out_result_types[target++]));
    }
  }
  return iree_ok_status();
}

static iree_status_t loom_boundary_projection_name_loop_components(
    loom_boundary_projection_plan_t* plan,
    const loom_boundary_projection_slot_t* candidate) {
  const loom_boundary_projection_schema_t* schema = &candidate->schema;
  if (!schema->component_name_suffixes) {
    return iree_ok_status();
  }
  for (uint16_t i = 0; i < schema->component_count; ++i) {
    IREE_RETURN_IF_ERROR(loom_rewriter_try_set_derived_value_name(
        &plan->rewriter, candidate->value_id, candidate->component_value_ids[i],
        schema->component_name_suffixes[i]));
  }
  return iree_ok_status();
}

static iree_status_t loom_boundary_projection_bind_loop_endpoint(
    loom_boundary_projection_plan_t* plan,
    loom_boundary_projection_function_t* function, iree_host_size_t candidate,
    const loom_value_id_t* values, uint16_t value_count) {
  if (candidate == IREE_HOST_SIZE_MAX) {
    return iree_ok_status();
  }
  loom_boundary_projection_slot_t* slot = &function->candidates[candidate];
  if (!slot->selected) {
    return iree_ok_status();
  }
  IREE_ASSERT_EQ(slot->schema.component_count, value_count);
  if (value_count != 0) {
    memcpy(slot->component_value_ids, values,
           value_count * sizeof(*slot->component_value_ids));
  }
  return loom_boundary_projection_name_loop_components(plan, slot);
}

static iree_status_t loom_boundary_projection_bind_loop_components(
    loom_boundary_projection_plan_t* plan,
    loom_boundary_projection_function_t* function,
    const loom_boundary_projection_loop_t* loop,
    const loom_loop_like_replacement_t* replacement) {
  for (uint16_t i = 0; i < loop->state_count; ++i) {
    const loom_boundary_projection_loop_state_t* state = &loop->states[i];
    if (!loom_boundary_projection_loop_state_is_selected(function, state)) {
      continue;
    }
    const uint16_t offset = loop->state_offsets[i];
    const uint16_t count = (uint16_t)(loop->state_offsets[i + 1] - offset);
    IREE_RETURN_IF_ERROR(loom_boundary_projection_bind_loop_endpoint(
        plan, function, state->result_candidate,
        replacement->results.values ? replacement->results.values + offset
                                    : NULL,
        count));
    IREE_RETURN_IF_ERROR(loom_boundary_projection_bind_loop_endpoint(
        plan, function, state->body_candidate,
        replacement->body_state.values ? replacement->body_state.values + offset
                                       : NULL,
        count));
    if (replacement->condition_entry) {
      IREE_RETURN_IF_ERROR(loom_boundary_projection_bind_loop_endpoint(
          plan, function, state->condition_candidate,
          replacement->condition_state.values
              ? replacement->condition_state.values + offset
              : NULL,
          count));
    }
  }
  return iree_ok_status();
}

static iree_status_t loom_boundary_projection_copy_loop_comments(
    loom_boundary_projection_plan_t* plan, const loom_op_t* source,
    const loom_op_t* target) {
  iree_host_size_t comment_count = 0;
  const iree_string_view_t* comments =
      loom_module_op_comments(plan->module, source, &comment_count);
  return comment_count == 0
             ? iree_ok_status()
             : loom_module_attach_op_comments(plan->module, target, comments,
                                              comment_count);
}

static iree_status_t loom_boundary_projection_loop_terminator_segments(
    loom_boundary_projection_plan_t* plan, const loom_op_t* source,
    uint16_t state_operand_offset, uint16_t target_state_count,
    const uint16_t** out_segment_counts, uint8_t* out_segment_count) {
  const loom_op_vtable_t* vtable = loom_op_vtable(plan->module, source);
  *out_segment_count = loom_op_vtable_operand_segment_count(vtable);
  *out_segment_counts = NULL;
  if (*out_segment_count == 0) {
    return iree_ok_status();
  }
  uint16_t* segment_counts = NULL;
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      plan->arena, *out_segment_count, sizeof(*segment_counts),
      (void**)&segment_counts));
  memcpy(segment_counts, loom_op_const_operand_segment_counts(source),
         *out_segment_count * sizeof(*segment_counts));
  uint16_t offset = 0;
  bool replaced = false;
  for (uint8_t i = 0; i < *out_segment_count; ++i) {
    if (offset == state_operand_offset) {
      segment_counts[i] = target_state_count;
      replaced = true;
      break;
    }
    offset = (uint16_t)(offset + segment_counts[i]);
  }
  IREE_ASSERT(replaced);
  *out_segment_counts = segment_counts;
  return iree_ok_status();
}

static iree_status_t loom_boundary_projection_build_loop_terminator(
    loom_boundary_projection_plan_t* plan, loom_op_t* parent,
    loom_region_t* target_region, const loom_op_t* source,
    uint16_t state_operand_offset, const loom_value_id_t* state_values,
    uint16_t state_count, loom_op_t** out_terminator) {
  const uint16_t operand_count = (uint16_t)(state_operand_offset + state_count);
  const uint16_t* segment_counts = NULL;
  uint8_t segment_count = 0;
  IREE_RETURN_IF_ERROR(loom_boundary_projection_loop_terminator_segments(
      plan, source, state_operand_offset, state_count, &segment_counts,
      &segment_count));

  loom_builder_t* builder = &plan->rewriter.builder;
  const loom_builder_ip_t saved =
      loom_builder_enter_region(builder, parent, target_region);
  iree_status_t status = iree_ok_status();
  if (segment_count != 0) {
    status = loom_builder_allocate_segmented_op(
        builder, source->kind, operand_count, segment_counts, segment_count,
        /*result_count=*/0, /*region_count=*/0, /*tied_result_count=*/0,
        source->attribute_count, source->location, out_terminator);
  } else {
    status = loom_builder_allocate_op(
        builder, source->kind, operand_count, /*result_count=*/0,
        /*region_count=*/0, /*tied_result_count=*/0, source->attribute_count,
        source->location, out_terminator);
  }
  if (iree_status_is_ok(status)) {
    loom_op_t* target = *out_terminator;
    target->instance_flags = source->instance_flags;
    target->traits = source->traits;
    target->flags |= source->flags & LOOM_OP_SOURCE_PRESENTATION_FLAG_MASK;
    if (state_operand_offset != 0) {
      memcpy(loom_op_operands(target), loom_op_const_operands(source),
             state_operand_offset * sizeof(loom_value_id_t));
    }
    if (state_count != 0) {
      memcpy(loom_op_operands(target) + state_operand_offset, state_values,
             state_count * sizeof(loom_value_id_t));
    }
    if (source->attribute_count != 0) {
      memcpy(loom_op_attrs(target), loom_op_const_attrs(source),
             source->attribute_count * sizeof(loom_attribute_t));
    }
    status = loom_builder_finalize_op(builder, target);
  }
  loom_builder_restore(builder, saved);
  IREE_RETURN_IF_ERROR(status);
  return loom_boundary_projection_copy_loop_comments(plan, source,
                                                     *out_terminator);
}

static iree_status_t loom_boundary_projection_build_loop_terminators(
    loom_boundary_projection_plan_t* plan,
    loom_boundary_projection_function_t* function,
    const loom_boundary_projection_loop_t* loop,
    const loom_loop_like_replacement_t* replacement,
    loom_op_t** out_condition_terminator, loom_op_t** out_body_terminator) {
  loom_value_id_t* state_values = NULL;
  if (loop->final_state_count != 0) {
    IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
        plan->arena, loop->final_state_count, sizeof(*state_values),
        (void**)&state_values));
  }

  if (loop->condition_terminator) {
    for (uint16_t i = 0; i < loop->state_count; ++i) {
      const loom_boundary_projection_loop_state_t* state = &loop->states[i];
      const uint16_t offset = loop->state_offsets[i];
      if (!loom_boundary_projection_loop_state_is_selected(function, state)) {
        state_values[offset] =
            loom_op_const_operands(loop->condition_terminator)[1 + i];
        continue;
      }
      const loom_boundary_projection_slot_t* candidate =
          &function->candidates[state->body_candidate];
      loom_builder_set_before(&plan->rewriter.builder,
                              loop->condition_terminator);
      IREE_RETURN_IF_ERROR(loom_boundary_projection_materialize_loop_source(
          plan, function, candidate, &state->condition_source,
          state_values ? state_values + offset : NULL));
    }
    IREE_RETURN_IF_ERROR(loom_boundary_projection_build_loop_terminator(
        plan, replacement->loop.op,
        loom_loop_like_condition_region(replacement->loop),
        loop->condition_terminator, /*state_operand_offset=*/1, state_values,
        loop->final_state_count, out_condition_terminator));
  } else {
    *out_condition_terminator = NULL;
  }

  for (uint16_t i = 0; i < loop->state_count; ++i) {
    const loom_boundary_projection_loop_state_t* state = &loop->states[i];
    const uint16_t offset = loop->state_offsets[i];
    if (!loom_boundary_projection_loop_state_is_selected(function, state)) {
      state_values[offset] = loom_op_const_operands(loop->body_terminator)[i];
      continue;
    }
    const iree_host_size_t entry_candidate = loop->condition_terminator
                                                 ? state->condition_candidate
                                                 : state->body_candidate;
    const loom_boundary_projection_slot_t* candidate =
        &function->candidates[entry_candidate];
    loom_builder_set_before(&plan->rewriter.builder, loop->body_terminator);
    IREE_RETURN_IF_ERROR(loom_boundary_projection_materialize_loop_source(
        plan, function, candidate, &state->backedge_source,
        state_values ? state_values + offset : NULL));
  }
  return loom_boundary_projection_build_loop_terminator(
      plan, replacement->loop.op, loom_loop_like_body(replacement->loop),
      loop->body_terminator, /*state_operand_offset=*/0, state_values,
      loop->final_state_count, out_body_terminator);
}

static iree_status_t loom_boundary_projection_map_unprojected_endpoint(
    loom_boundary_projection_plan_t* plan, loom_ir_remap_t* remap,
    loom_value_id_t source, loom_value_id_t target) {
  IREE_RETURN_IF_ERROR(loom_ir_remap_map_value(remap, source, target));
  IREE_RETURN_IF_ERROR(
      loom_rewriter_move_value_name(&plan->rewriter, source, target));
  return loom_rewriter_replace_all_uses_with(&plan->rewriter, source, target);
}

static iree_status_t loom_boundary_projection_realize_loop_endpoint(
    loom_boundary_projection_plan_t* plan,
    loom_boundary_projection_function_t* function,
    const loom_boundary_projection_loop_t* loop,
    const loom_value_id_t* target_values, bool condition_endpoint,
    loom_op_t* target_terminator) {
  loom_ir_remap_t remap = {0};
  IREE_RETURN_IF_ERROR(
      loom_boundary_projection_loop_initialize_remap(plan, &remap));
  for (uint16_t i = 0; i < loop->state_count; ++i) {
    const loom_boundary_projection_loop_state_t* state = &loop->states[i];
    if (loom_boundary_projection_loop_state_is_selected(function, state)) {
      continue;
    }
    const loom_value_id_t source =
        condition_endpoint ? state->condition_value_id : state->body_value_id;
    IREE_RETURN_IF_ERROR(loom_boundary_projection_map_unprojected_endpoint(
        plan, &remap, source, target_values[loop->state_offsets[i]]));
  }

  for (uint16_t i = 0; i < loop->state_count; ++i) {
    const loom_boundary_projection_loop_state_t* state = &loop->states[i];
    if (!loom_boundary_projection_loop_state_is_selected(function, state)) {
      continue;
    }
    const iree_host_size_t candidate_index =
        condition_endpoint ? state->condition_candidate : state->body_candidate;
    loom_boundary_projection_slot_t* candidate =
        &function->candidates[candidate_index];
    loom_type_t logical_type = loom_type_none();
    IREE_RETURN_IF_ERROR(
        loom_ir_remap_type(&remap, candidate->logical_type, &logical_type));
    loom_builder_set_before(&plan->rewriter.builder, target_terminator);
    if (candidate->schema.destination_mode ==
        LOOM_BOUNDARY_PROJECTION_DESTINATION_RECONSTRUCT) {
      IREE_RETURN_IF_ERROR(candidate->schema.rule->transport.reconstruct(
          candidate->schema.rule, plan, function, candidate, logical_type,
          target_terminator->location, &candidate->replacement_value_id));
      IREE_RETURN_IF_ERROR(
          loom_rewriter_move_value_name(&plan->rewriter, candidate->value_id,
                                        candidate->replacement_value_id));
      IREE_RETURN_IF_ERROR(loom_rewriter_replace_all_uses_with(
          &plan->rewriter, candidate->value_id,
          candidate->replacement_value_id));
    } else {
      IREE_RETURN_IF_ERROR(candidate->schema.rule->transport.eliminate(
          candidate->schema.rule, plan, function, candidate, logical_type,
          target_terminator->location));
      IREE_ASSERT(
          !loom_module_value_has_uses(plan->module, candidate->value_id));
    }
  }
  return iree_ok_status();
}

static iree_status_t loom_boundary_projection_realize_loop_results(
    loom_boundary_projection_plan_t* plan,
    loom_boundary_projection_function_t* function,
    const loom_boundary_projection_loop_t* loop,
    const loom_loop_like_replacement_t* replacement) {
  loom_ir_remap_t remap = {0};
  IREE_RETURN_IF_ERROR(
      loom_boundary_projection_loop_initialize_remap(plan, &remap));
  for (uint16_t i = 0; i < loop->state_count; ++i) {
    const loom_boundary_projection_loop_state_t* state = &loop->states[i];
    if (loom_boundary_projection_loop_state_is_selected(function, state)) {
      continue;
    }
    IREE_RETURN_IF_ERROR(loom_boundary_projection_map_unprojected_endpoint(
        plan, &remap, state->result_value_id,
        replacement->results.values[loop->state_offsets[i]]));
  }

  for (uint16_t i = 0; i < loop->state_count; ++i) {
    const loom_boundary_projection_loop_state_t* state = &loop->states[i];
    if (!loom_boundary_projection_loop_state_is_selected(function, state)) {
      continue;
    }
    loom_boundary_projection_slot_t* candidate =
        &function->candidates[state->result_candidate];
    loom_type_t logical_type = loom_type_none();
    IREE_RETURN_IF_ERROR(
        loom_ir_remap_type(&remap, candidate->logical_type, &logical_type));
    loom_builder_set_after(&plan->rewriter.builder, replacement->loop.op);
    if (candidate->schema.destination_mode ==
        LOOM_BOUNDARY_PROJECTION_DESTINATION_RECONSTRUCT) {
      IREE_RETURN_IF_ERROR(candidate->schema.rule->transport.reconstruct(
          candidate->schema.rule, plan, function, candidate, logical_type,
          replacement->loop.op->location, &candidate->replacement_value_id));
      IREE_RETURN_IF_ERROR(
          loom_rewriter_move_value_name(&plan->rewriter, candidate->value_id,
                                        candidate->replacement_value_id));
      IREE_RETURN_IF_ERROR(loom_rewriter_replace_all_uses_with(
          &plan->rewriter, candidate->value_id,
          candidate->replacement_value_id));
    } else {
      IREE_RETURN_IF_ERROR(candidate->schema.rule->transport.eliminate(
          candidate->schema.rule, plan, function, candidate, logical_type,
          replacement->loop.op->location));
      IREE_ASSERT(
          !loom_module_value_has_uses(plan->module, candidate->value_id));
    }
  }
  return iree_ok_status();
}

static iree_status_t loom_boundary_projection_move_loop_region(
    loom_boundary_projection_plan_t* plan, loom_block_t* source_block,
    loom_op_t* target_terminator) {
  loom_op_t* op = source_block->first_op;
  while (op) {
    loom_op_t* next = op->next_op;
    IREE_RETURN_IF_ERROR(
        loom_rewriter_move_before(&plan->rewriter, op, target_terminator));
    op = next;
  }
  return iree_ok_status();
}

static iree_status_t loom_boundary_projection_apply_loop(
    loom_boundary_projection_plan_t* plan,
    loom_boundary_projection_function_t* function,
    loom_boundary_projection_loop_t* loop) {
  loom_value_id_t* initial_values = NULL;
  loom_type_t* result_types = NULL;
  loom_value_id_t* reserved_results = NULL;
  if (loop->final_state_count != 0) {
    IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
        plan->arena, loop->final_state_count, sizeof(*initial_values),
        (void**)&initial_values));
    IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
        plan->arena, loop->final_state_count, sizeof(*result_types),
        (void**)&result_types));
    IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
        plan->arena, loop->final_state_count, sizeof(*reserved_results),
        (void**)&reserved_results));
  }
  IREE_RETURN_IF_ERROR(loom_boundary_projection_materialize_initial_state(
      plan, function, loop, initial_values));

  loom_builder_set_before(&plan->rewriter.builder, loop->loop.op);
  if (loop->final_state_count != 0) {
    IREE_RETURN_IF_ERROR(loom_builder_reserve_results(
        &plan->rewriter.builder, loop->final_state_count, reserved_results));
    IREE_RETURN_IF_ERROR(loom_boundary_projection_build_loop_result_types(
        plan, function, loop, reserved_results, result_types));
  }
  const loom_loop_like_replacement_state_t replacement_state = {
      .initial_values =
          {
              .values = initial_values,
              .count = loop->final_state_count,
          },
      .source_header_offsets = loop->state_offsets,
      .result_types = result_types,
      .result_count = loop->final_state_count,
      .source_result_offsets = loop->state_offsets,
  };
  loom_loop_like_replacement_t replacement = {0};
  IREE_RETURN_IF_ERROR(loom_loop_like_build_replacement(
      &plan->rewriter.builder, loop->loop, &replacement_state, plan->arena,
      &replacement));
  for (uint16_t i = 0; i < loop->final_state_count; ++i) {
    IREE_ASSERT_EQ(replacement.results.values[i], reserved_results[i]);
  }
  IREE_RETURN_IF_ERROR(loom_boundary_projection_bind_loop_components(
      plan, function, loop, &replacement));

  loom_op_t* condition_terminator = NULL;
  loom_op_t* body_terminator = NULL;
  IREE_RETURN_IF_ERROR(loom_boundary_projection_build_loop_terminators(
      plan, function, loop, &replacement, &condition_terminator,
      &body_terminator));

  // Outgoing source recipes have already been materialized into the new
  // terminators. Retire the old terminators before destination elimination so
  // eliminative rules can remove complete source chains without a temporary
  // aggregate use keeping their final operation alive.
  if (loop->condition_terminator) {
    IREE_RETURN_IF_ERROR(
        loom_rewriter_erase(&plan->rewriter, loop->condition_terminator));
  }
  IREE_RETURN_IF_ERROR(
      loom_rewriter_erase(&plan->rewriter, loop->body_terminator));

  if (replacement.condition_entry) {
    IREE_RETURN_IF_ERROR(loom_boundary_projection_realize_loop_endpoint(
        plan, function, loop, replacement.condition_state.values,
        /*condition_endpoint=*/true, condition_terminator));
  }
  IREE_RETURN_IF_ERROR(loom_boundary_projection_realize_loop_endpoint(
      plan, function, loop, replacement.body_state.values,
      /*condition_endpoint=*/false, body_terminator));
  const loom_value_id_t source_iv = loom_loop_like_iv(loop->loop);
  if (source_iv != LOOM_VALUE_ID_INVALID) {
    const loom_value_id_t target_iv = loom_loop_like_iv(replacement.loop);
    IREE_RETURN_IF_ERROR(
        loom_rewriter_move_value_name(&plan->rewriter, source_iv, target_iv));
    IREE_RETURN_IF_ERROR(loom_rewriter_replace_all_uses_with(
        &plan->rewriter, source_iv, target_iv));
  }

  if (replacement.condition_entry) {
    loom_block_t* source_condition =
        loom_region_entry_block(loom_loop_like_condition_region(loop->loop));
    IREE_RETURN_IF_ERROR(loom_boundary_projection_move_loop_region(
        plan, source_condition, condition_terminator));
  }
  loom_block_t* source_body =
      loom_region_entry_block(loom_loop_like_body(loop->loop));
  IREE_RETURN_IF_ERROR(loom_boundary_projection_move_loop_region(
      plan, source_body, body_terminator));
  IREE_RETURN_IF_ERROR(loom_boundary_projection_realize_loop_results(
      plan, function, loop, &replacement));
  IREE_RETURN_IF_ERROR(loom_rewriter_erase(&plan->rewriter, loop->loop.op));
  ++plan->loops_rewritten;
  return iree_ok_status();
}

iree_status_t loom_boundary_projection_apply_loops(
    loom_boundary_projection_plan_t* plan,
    loom_boundary_projection_function_t* function) {
  for (iree_host_size_t i = 0; i < function->loop_count; ++i) {
    loom_boundary_projection_loop_t* loop = &function->loops[i];
    if (!loop->selected ||
        iree_any_bit_set(loop->loop.op->flags, LOOM_OP_FLAG_DEAD)) {
      continue;
    }
    IREE_RETURN_IF_ERROR(
        loom_boundary_projection_apply_loop(plan, function, loop));
  }
  return iree_ok_status();
}
