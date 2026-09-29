// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/codegen/low/transforms/register_constraints.h"

#include <string.h>

#include "loom/codegen/low/builder.h"
#include "loom/codegen/low/diagnostics.h"
#include "loom/codegen/low/function.h"
#include "loom/codegen/low/memory_access.h"
#include "loom/codegen/low/pipeline/pass_environment.h"
#include "loom/codegen/low/representation_binding.h"
#include "loom/codegen/low/target_binding.h"
#include "loom/error/error_catalog.h"
#include "loom/ir/module.h"
#include "loom/ops/low/ops.h"
#include "loom/pass/value_facts.h"
#include "loom/rewrite/rewriter.h"
#include "loom/target/function_version.h"

static const loom_pass_info_t kPassInfo = {
    .name = IREE_SVL("low-materialize-register-constraints"),
    .description =
        IREE_SVL("Materialize independent instruction register ties."),
    .kind = LOOM_PASS_FUNCTION,
};

const loom_pass_info_t* loom_low_materialize_register_constraints_pass_info(
    void) {
  return &kPassInfo;
}

typedef struct loom_low_register_constraints_state_t {
  // Pass invocation owning diagnostics, scratch storage, and invalidation.
  loom_pass_t* pass;
  // Resolved instruction vocabulary for this function.
  const loom_low_descriptor_set_t* descriptor_set;
  // Rewriter owning Low copies and packet replacement.
  loom_rewriter_t rewriter;
  // Function name used by constraint diagnostics.
  iree_string_view_t function_name;
  // True once a user constraint conflict has been diagnosed.
  bool rejected;
  // True once any packet has been replaced.
  bool changed;
} loom_low_register_constraints_state_t;

static const loom_tied_result_t* loom_low_register_constraints_find_tie(
    const loom_op_t* op, uint16_t result_index, uint16_t operand_index) {
  const loom_tied_result_t* ties = loom_op_tied_results(op);
  for (uint16_t i = 0; i < op->tied_result_count; ++i) {
    if (ties[i].result_index == result_index ||
        ties[i].operand_index == operand_index) {
      return &ties[i];
    }
  }
  return NULL;
}

static iree_status_t loom_low_register_constraints_packet(
    loom_low_register_constraints_state_t* state, loom_op_t* op) {
  loom_low_descriptor_packet_t packet;
  loom_low_descriptor_packet_initialize(state->descriptor_set, op, &packet);
  if (packet.kind != LOOM_LOW_DESCRIPTOR_PACKET_OP) {
    return iree_ok_status();
  }
  const loom_low_descriptor_t* descriptor = packet.descriptor;
  if (descriptor->constraint_count == 0) {
    return iree_ok_status();
  }
  const loom_low_operand_t* fields =
      &state->descriptor_set->operands[descriptor->operand_start];
  const loom_low_constraint_t* constraints =
      &state->descriptor_set->constraints[descriptor->constraint_start];
  uint16_t missing_count = 0;
  for (uint16_t i = 0; i < descriptor->constraint_count; ++i) {
    const loom_low_constraint_t* constraint = &constraints[i];
    if (constraint->kind != LOOM_LOW_CONSTRAINT_KIND_TIED) {
      continue;
    }
    const uint16_t result_index =
        fields[constraint->lhs_operand_index].source_value_index;
    const uint16_t operand_index =
        fields[constraint->rhs_operand_index].source_value_index;
    const loom_tied_result_t* tie =
        loom_low_register_constraints_find_tie(op, result_index, operand_index);
    if (tie == NULL) {
      ++missing_count;
    } else if (tie->result_index != result_index ||
               tie->operand_index != operand_index) {
      const loom_diagnostic_param_t params[] = {
          loom_param_string(state->function_name),
          loom_param_string(loom_low_descriptor_set_string(
              state->descriptor_set, descriptor->key_string_ref)),
          loom_param_with_field_ref(
              loom_param_u32(tie->result_index),
              loom_diagnostic_field_ref(LOOM_DIAGNOSTIC_FIELD_RESULT,
                                        tie->result_index)),
          loom_param_u32(tie->operand_index),
          loom_param_u32(result_index),
          loom_param_with_field_ref(
              loom_param_u32(operand_index),
              loom_diagnostic_field_ref(LOOM_DIAGNOSTIC_FIELD_OPERAND,
                                        operand_index)),
      };
      const loom_diagnostic_emission_t emission = {
          .op = op,
          .error = LOOM_ERR_TARGET_092,
          .params = params,
          .param_count = IREE_ARRAYSIZE(params),
      };
      state->rejected = true;
      return iree_diagnostic_emit(state->pass->diagnostic_emitter, &emission);
    }
  }
  if (missing_count == 0) {
    return iree_ok_status();
  }

  loom_module_t* module = state->rewriter.builder.module;
  iree_arena_allocator_t* arena = state->pass->arena;
  loom_value_id_t* operands = NULL;
  loom_type_t* result_types = NULL;
  loom_tied_result_t* ties = NULL;
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      arena, op->operand_count, sizeof(*operands), (void**)&operands));
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      arena, op->result_count, sizeof(*result_types), (void**)&result_types));
  IREE_RETURN_IF_ERROR(
      iree_arena_allocate_array(arena, op->tied_result_count + missing_count,
                                sizeof(*ties), (void**)&ties));
  memcpy(operands, loom_op_const_operands(op),
         op->operand_count * sizeof(*operands));
  memcpy(ties, loom_op_tied_results(op), op->tied_result_count * sizeof(*ties));
  for (uint16_t i = 0; i < op->result_count; ++i) {
    result_types[i] =
        loom_module_value_type(module, loom_op_const_results(op)[i]);
  }

  const loom_value_id_t value_checkpoint =
      loom_rewriter_value_checkpoint(&state->rewriter);
  loom_builder_t* builder = &state->rewriter.builder;
  const loom_builder_ip_t saved_ip = loom_builder_save(builder);
  loom_builder_set_before(builder, op);
  uint16_t tie_count = op->tied_result_count;
  iree_status_t status = iree_ok_status();
  for (uint16_t i = 0;
       iree_status_is_ok(status) && i < descriptor->constraint_count; ++i) {
    const loom_low_constraint_t* constraint = &constraints[i];
    if (constraint->kind != LOOM_LOW_CONSTRAINT_KIND_TIED) {
      continue;
    }
    const uint16_t result_index =
        fields[constraint->lhs_operand_index].source_value_index;
    const uint16_t operand_index =
        fields[constraint->rhs_operand_index].source_value_index;
    if (loom_low_register_constraints_find_tie(op, result_index,
                                               operand_index) != NULL) {
      continue;
    }
    loom_op_t* copy = NULL;
    status =
        loom_low_copy_build(builder, operands[operand_index], false,
                            result_types[result_index], op->location, &copy);
    if (iree_status_is_ok(status)) {
      operands[operand_index] = loom_low_copy_result(copy);
      ties[tie_count++] = (loom_tied_result_t){
          .result_index = result_index,
          .operand_index = operand_index,
      };
    }
  }
  loom_op_t* replacement = NULL;
  if (iree_status_is_ok(status)) {
    status = loom_low_build_resolved_descriptor_op(
        builder, state->descriptor_set, descriptor, op->instance_flags,
        operands, op->operand_count, loom_low_op_attrs(op), result_types,
        op->result_count, ties, tie_count, op->location, &replacement);
  }
  loom_builder_restore(builder, saved_ip);
  IREE_RETURN_IF_ERROR(status);

  loom_target_function_version_t* version =
      loom_target_function_version_cast(state->pass->function_version);
  if (version != NULL) {
    IREE_RETURN_IF_ERROR(loom_low_memory_access_map_replace(
        version->memory_accesses, op, replacement));
  }
  IREE_RETURN_IF_ERROR(loom_rewriter_preserve_result_names_on_new_values(
      &state->rewriter, op, loom_op_results(replacement), op->result_count,
      value_checkpoint));
  IREE_RETURN_IF_ERROR(loom_rewriter_replace_all_uses_and_erase(
      &state->rewriter, op, loom_op_results(replacement), op->result_count));
  state->changed = true;
  loom_pass_mark_changed(state->pass);
  return iree_ok_status();
}

static iree_status_t loom_low_register_constraints_region(
    loom_low_register_constraints_state_t* state, loom_region_t* region) {
  iree_status_t status = iree_ok_status();
  for (uint16_t i = 0;
       iree_status_is_ok(status) && !state->rejected && i < region->block_count;
       ++i) {
    loom_block_t* block = region->blocks[i];
    for (loom_op_t* op = block->first_op;
         iree_status_is_ok(status) && !state->rejected && op != NULL;) {
      loom_op_t* next = op->next_op;
      if (!iree_any_bit_set(op->flags, LOOM_OP_FLAG_DEAD)) {
        for (uint8_t j = 0; iree_status_is_ok(status) && !state->rejected &&
                            j < op->region_count;
             ++j) {
          loom_region_t* child = loom_op_regions(op)[j];
          if (child != NULL) {
            status = loom_low_register_constraints_region(state, child);
          }
        }
        if (iree_status_is_ok(status) && !state->rejected) {
          status = loom_low_register_constraints_packet(state, op);
        }
      }
      op = next;
    }
  }
  return status;
}

iree_status_t loom_low_materialize_register_constraints_run(
    loom_pass_t* pass, loom_module_t* module, loom_func_like_t function) {
  if (!loom_low_function_def_isa(function.op)) {
    return iree_ok_status();
  }
  loom_symbol_fact_table_t symbol_facts;
  loom_symbol_fact_table_initialize(&symbol_facts, pass->arena);
  loom_low_resolved_target_t target = {0};
  IREE_RETURN_IF_ERROR(loom_low_resolve_function_target(
      module, &symbol_facts, function.op,
      loom_target_function_version_target_facts(pass->function_version),
      loom_low_pass_capability_descriptor_registry(
          loom_low_pass_capability_from_pass(pass)),
      pass->diagnostic_emitter, &target));
  if (target.descriptor_set == NULL) {
    return iree_ok_status();
  }
  loom_low_register_constraints_state_t state = {
      .pass = pass,
      .descriptor_set = target.descriptor_set,
      .function_name = loom_low_diagnostic_function_name(module, function.op),
  };
  loom_rewriter_initialize(&state.rewriter, module, pass->arena);
  iree_status_t status = loom_low_register_constraints_region(
      &state, loom_func_like_body(function));
  loom_rewriter_deinitialize(&state.rewriter);
  if (state.changed) {
    loom_pass_value_fact_owner_invalidate(pass->value_facts);
  }
  return status;
}
