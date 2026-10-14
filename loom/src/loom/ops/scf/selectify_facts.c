// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/ops/scf/selectify_facts.h"

#include <string.h>

#include "iree/base/internal/arena.h"
#include "loom/ir/context.h"
#include "loom/ir/module.h"
#include "loom/ir/value_refs.h"
#include "loom/ops/op_defs.h"
#include "loom/ops/scf/ops.h"
#include "loom/rewrite/remap.h"
#include "loom/util/fact_extensions.h"
#include "loom/util/fact_table.h"

bool loom_scf_if_op_is_strippable_fact_identity(const loom_module_t* module,
                                                const loom_op_t* op,
                                                loom_trait_flags_t traits) {
  if (loom_traits_are_fact_identity(traits) &&
      iree_any_bit_set(traits, LOOM_TRAIT_PURE) &&
      !iree_any_bit_set(traits, LOOM_TRAIT_HINT | LOOM_TRAIT_CONVERGENT |
                                    LOOM_TRAIT_OBSERVABLE_EFFECT) &&
      !loom_traits_may_read(traits) && !loom_traits_may_write(traits) &&
      op->operand_count == 1 && op->result_count == 1) {
    return loom_type_equal(
        loom_module_value_type(module, loom_op_const_operands(op)[0]),
        loom_module_value_type(module, loom_op_const_results(op)[0]));
  }
  return false;
}

typedef struct loom_scf_if_fact_preview_t {
  // Module containing the conditional and candidate operations.
  const loom_module_t* module;
  // Facts computed for the current function before selectification.
  const loom_value_fact_table_t* source_facts;
  // Scratch facts recomputed after removing branch-local identities.
  loom_value_fact_table_t facts;
  // Sparse replacement map from stripped identity results to their sources.
  loom_ir_remap_t stripped_values;
  // Invocation-local storage for remaps, cloned operations, and facts.
  iree_arena_allocator_t* arena;
  // False when a value reference outside ordinary operands cannot be modeled.
  bool can_preserve_facts;
} loom_scf_if_fact_preview_t;

static iree_host_size_t loom_scf_if_preview_op_storage_size(
    const loom_module_t* module, const loom_op_t* op) {
  const loom_op_vtable_t* vtable = loom_op_vtable(module, op);
  const uint8_t operand_segment_count =
      loom_op_vtable_operand_segment_count(vtable);
  if (op->attribute_count == 0 && operand_segment_count == 0) {
    return (iree_host_size_t)((const uint8_t*)(loom_op_tied_results(op) +
                                               op->tied_result_count) -
                              (const uint8_t*)op);
  }
  if (operand_segment_count != 0) {
    return (
        iree_host_size_t)((const uint8_t*)(loom_op_const_operand_segment_counts(
                                               op) +
                                           operand_segment_count) -
                          (const uint8_t*)op);
  }
  return (iree_host_size_t)((const uint8_t*)(loom_op_attribute_owners(op) +
                                             op->attribute_count) -
                            (const uint8_t*)op);
}

static iree_status_t loom_scf_if_preview_clone_remapped_op(
    loom_scf_if_fact_preview_t* preview, const loom_op_t* source_op,
    loom_op_t** out_op) {
  const iree_host_size_t storage_size =
      loom_scf_if_preview_op_storage_size(preview->module, source_op);
  loom_op_t* op = NULL;
  IREE_RETURN_IF_ERROR(
      iree_arena_allocate(preview->arena, storage_size, (void**)&op));
  memcpy(op, source_op, storage_size);
  loom_value_id_t* operands = loom_op_operands(op);
  for (uint16_t i = 0; i < op->operand_count; ++i) {
    loom_value_id_t replacement = LOOM_VALUE_ID_INVALID;
    if (loom_ir_remap_try_lookup_value(&preview->stripped_values, operands[i],
                                       &replacement)) {
      operands[i] = replacement;
    }
  }
  *out_op = op;
  return iree_ok_status();
}

static iree_status_t loom_scf_if_preview_seed_value(loom_value_id_t value_id,
                                                    void* user_data) {
  loom_scf_if_fact_preview_t* preview = user_data;
  loom_value_id_t source_value_id = LOOM_VALUE_ID_INVALID;
  if (loom_ir_remap_try_lookup_value(&preview->stripped_values, value_id,
                                     &source_value_id)) {
    value_id = source_value_id;
  }
  if (value_id == LOOM_VALUE_ID_INVALID ||
      value_id >= preview->module->values.count ||
      loom_value_fact_table_has_entry(&preview->facts, value_id)) {
    return iree_ok_status();
  }
  const loom_value_fact_table_view_t source = {
      .table = preview->source_facts,
      .value_ids = &value_id,
      .value_count = 1,
  };
  return loom_value_fact_table_clone_values(&preview->facts, source,
                                            preview->module);
}

static iree_status_t loom_scf_if_preview_prepare_block(
    loom_scf_if_fact_preview_t* preview, const loom_block_t* block,
    const loom_op_t* yield) {
  for (const loom_op_t* child_op = block->first_op; child_op != yield;
       child_op = child_op->next_op) {
    const loom_trait_flags_t traits =
        loom_op_effective_traits(preview->module, child_op);
    if (!loom_scf_if_op_is_strippable_fact_identity(preview->module, child_op,
                                                    traits)) {
      continue;
    }
    const loom_value_id_t result = loom_op_const_results(child_op)[0];
    const loom_value_t* result_value =
        loom_module_value(preview->module, result);
    if (loom_value_has_attribute_uses(result_value) ||
        loom_module_value_has_type_uses(preview->module, result)) {
      preview->can_preserve_facts = false;
      return iree_ok_status();
    }
    loom_value_id_t source = loom_op_const_operands(child_op)[0];
    loom_value_id_t remapped_source = LOOM_VALUE_ID_INVALID;
    if (loom_ir_remap_try_lookup_value(&preview->stripped_values, source,
                                       &remapped_source)) {
      source = remapped_source;
    }
    IREE_RETURN_IF_ERROR(
        loom_ir_remap_map_value(&preview->stripped_values, result, source));
  }
  return iree_ok_status();
}

static iree_status_t loom_scf_if_preview_compute_block(
    loom_scf_if_fact_preview_t* preview, const loom_block_t* block,
    const loom_op_t* yield, loom_value_id_t* out_yield_values,
    loom_value_facts_t* out_yield_facts) {
  for (const loom_op_t* child_op = block->first_op; child_op != yield;
       child_op = child_op->next_op) {
    const loom_trait_flags_t traits =
        loom_op_effective_traits(preview->module, child_op);
    if (loom_scf_if_op_is_strippable_fact_identity(preview->module, child_op,
                                                   traits)) {
      continue;
    }
    IREE_RETURN_IF_ERROR(loom_op_walk_subtree_value_refs(
        preview->module, child_op, loom_scf_if_preview_seed_value, preview));
    loom_op_t* preview_op = NULL;
    IREE_RETURN_IF_ERROR(
        loom_scf_if_preview_clone_remapped_op(preview, child_op, &preview_op));
    IREE_RETURN_IF_ERROR(loom_value_fact_table_compute_op(
        &preview->facts, preview->module, preview_op));
  }

  const loom_value_slice_t yield_values = loom_scf_yield_values(yield);
  for (iree_host_size_t i = 0; i < yield_values.count; ++i) {
    loom_value_id_t value = yield_values.values[i];
    loom_value_id_t remapped_value = LOOM_VALUE_ID_INVALID;
    if (loom_ir_remap_try_lookup_value(&preview->stripped_values, value,
                                       &remapped_value)) {
      value = remapped_value;
    }
    IREE_RETURN_IF_ERROR(loom_scf_if_preview_seed_value(value, preview));
    out_yield_values[i] = value;
    out_yield_facts[i] = loom_value_fact_table_lookup(&preview->facts, value);
  }
  return iree_ok_status();
}

static iree_status_t loom_scf_if_stripped_facts_preserve_results_impl(
    loom_op_t* op, loom_rewriter_t* rewriter, const loom_block_t* then_block,
    const loom_op_t* then_yield, const loom_block_t* else_block,
    const loom_op_t* else_yield, iree_arena_allocator_t* arena,
    bool* out_preserves_facts) {
  *out_preserves_facts = false;
  loom_scf_if_fact_preview_t preview = {
      .module = rewriter->module,
      .source_facts = rewriter->fact_table,
      .arena = arena,
      .can_preserve_facts = true,
  };
  const loom_ir_remap_options_t remap_options = {
      .allow_unmapped_values = true,
      .value_map_kind = LOOM_IR_REMAP_VALUE_MAP_SPARSE,
  };
  IREE_RETURN_IF_ERROR(
      loom_ir_remap_initialize(rewriter->module, rewriter->module, arena,
                               &remap_options, &preview.stripped_values));
  IREE_RETURN_IF_ERROR(
      loom_scf_if_preview_prepare_block(&preview, then_block, then_yield));
  IREE_RETURN_IF_ERROR(
      loom_scf_if_preview_prepare_block(&preview, else_block, else_yield));
  if (!preview.can_preserve_facts) {
    return iree_ok_status();
  }
  if (preview.source_facts == NULL) {
    *out_preserves_facts = true;
    return iree_ok_status();
  }

  IREE_RETURN_IF_ERROR(
      loom_value_fact_table_initialize(&preview.facts, arena, 0));
  preview.facts.context = preview.source_facts->context;
  preview.facts.context.table = &preview.facts;
  preview.facts.context.refine_operands.fn = NULL;
  preview.facts.context.refine_operands.user_data = NULL;
  preview.facts.context.result_facts_changed = NULL;

  const iree_host_size_t arm_result_count = 2 * op->result_count;
  loom_value_id_t* arm_values = NULL;
  loom_value_facts_t* arm_facts = NULL;
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      arena, arm_result_count, sizeof(*arm_values), (void**)&arm_values));
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      arena, arm_result_count, sizeof(*arm_facts), (void**)&arm_facts));
  loom_value_id_t* else_values = arm_values + op->result_count;
  loom_value_facts_t* else_facts = arm_facts + op->result_count;
  IREE_RETURN_IF_ERROR(loom_scf_if_preview_compute_block(
      &preview, then_block, then_yield, arm_values, arm_facts));
  IREE_RETURN_IF_ERROR(loom_scf_if_preview_compute_block(
      &preview, else_block, else_yield, else_values, else_facts));

  const loom_value_id_t condition = loom_scf_if_condition(op);
  IREE_RETURN_IF_ERROR(loom_scf_if_preview_seed_value(condition, &preview));
  const loom_value_facts_t condition_facts =
      loom_value_fact_table_lookup(&preview.facts, condition);
  const loom_value_id_t* results = loom_op_const_results(op);
  for (uint16_t i = 0; i < op->result_count; ++i) {
    const loom_type_t result_type =
        loom_module_value_type(rewriter->module, results[i]);
    loom_value_facts_t joined_facts = loom_value_facts_unknown();
    IREE_RETURN_IF_ERROR(loom_value_fact_table_meet_for_type(
        &preview.facts, rewriter->module, result_type, &preview.facts,
        arm_facts[i], &preview.facts, else_facts[i], &joined_facts));
    if (arm_values[i] != else_values[i]) {
      loom_value_facts_propagate_binary_distribution(
          joined_facts, condition_facts, &joined_facts);
      if (loom_value_facts_is_lane_varying(joined_facts)) {
        loom_value_facts_mark_lane_distribution_for_type(result_type,
                                                         &joined_facts);
      }
    }
    const loom_value_facts_t source_facts =
        loom_value_fact_table_lookup(preview.source_facts, results[i]);
    loom_value_facts_t preserved_facts = loom_value_facts_unknown();
    IREE_RETURN_IF_ERROR(loom_value_fact_table_meet_for_type(
        &preview.facts, rewriter->module, result_type, preview.source_facts,
        source_facts, &preview.facts, joined_facts, &preserved_facts));
    if (!loom_value_fact_table_facts_equal_for_type(
            rewriter->module, result_type, preview.source_facts, source_facts,
            &preview.facts, preserved_facts)) {
      return iree_ok_status();
    }
  }

  *out_preserves_facts = true;
  return iree_ok_status();
}

iree_status_t loom_scf_if_stripped_facts_preserve_results(
    loom_op_t* op, loom_rewriter_t* rewriter, const loom_block_t* then_block,
    const loom_op_t* then_yield, const loom_block_t* else_block,
    const loom_op_t* else_yield, bool* out_preserves_facts) {
  iree_arena_allocator_t arena;
  iree_arena_initialize(rewriter->arena->block_pool, &arena);
  iree_status_t status = loom_scf_if_stripped_facts_preserve_results_impl(
      op, rewriter, then_block, then_yield, else_block, else_yield, &arena,
      out_preserves_facts);
  iree_arena_deinitialize(&arena);
  return status;
}
