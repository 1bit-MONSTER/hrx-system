// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/util/fact_induction.h"

#include "loom/ir/module.h"
#include "loom/ops/cfg/ops.h"
#include "loom/ops/index/ops.h"
#include "loom/target/facts.h"
#include "loom/util/fact_cfg.h"
#include "loom/util/fact_table.h"

static const loom_op_t* loom_value_fact_induction_defining_op(
    const loom_module_t* module, loom_value_id_t value_id) {
  const loom_value_t* value = loom_module_value(module, value_id);
  return loom_value_is_block_arg(value) ? NULL : loom_value_def_op(value);
}

static bool loom_value_fact_induction_is_invariant(
    const loom_module_t* module, const loom_cfg_loop_nest_t* loops,
    uint16_t loop_index, loom_value_id_t value_id) {
  const loom_value_t* value = loom_module_value(module, value_id);
  const loom_block_t* block = NULL;
  if (loom_value_is_block_arg(value)) {
    block = loom_value_def_block(value);
  } else {
    const loom_op_t* op = loom_value_def_op(value);
    if (loom_index_constant_isa(op)) {
      return true;
    }
    block = op->parent_block;
  }
  return block->parent_region != loops->graph->region ||
         !loom_cfg_loop_nest_contains(loops, loop_index, block->region_index);
}

static bool loom_value_fact_induction_compare_flags(
    const loom_op_t* compare, loom_loop_bound_flags_t* out_flags) {
  loom_loop_bound_flags_t bound_flags = LOOM_LOOP_BOUND_NONE;
  switch (loom_index_cmp_predicate(compare)) {
    case LOOM_INDEX_CMP_PREDICATE_SLT:
      bound_flags = LOOM_LOOP_BOUND_SIGNED;
      break;
    case LOOM_INDEX_CMP_PREDICATE_SLE:
      bound_flags = LOOM_LOOP_BOUND_SIGNED | LOOM_LOOP_BOUND_INCLUSIVE;
      break;
    case LOOM_INDEX_CMP_PREDICATE_ULT:
      break;
    case LOOM_INDEX_CMP_PREDICATE_ULE:
      bound_flags = LOOM_LOOP_BOUND_INCLUSIVE;
      break;
    default:
      return false;
  }
  *out_flags = bound_flags;
  return true;
}

static loom_value_fact_induction_t loom_value_fact_induction_recognize(
    const loom_value_fact_table_t* table, const loom_module_t* module,
    const loom_cfg_loop_nest_t* loops, uint16_t loop_index) {
  const loom_value_fact_induction_t unknown = {
      .value = LOOM_VALUE_ID_INVALID,
  };
  const loom_cfg_natural_loop_t* loop = &loops->loops[loop_index];
  if (loop->entries.count != 1 || loop->backedges.count != 1 ||
      loop->exits.count != 1) {
    return unknown;
  }
  const loom_cfg_graph_t* graph = loops->graph;
  const loom_cfg_edge_info_t* exit = &graph->edges[loop->exits.unique_index];
  const loom_block_t* header = graph->blocks[loop->header_index].block;
  const loom_op_t* terminator = header->last_op;
  if (exit->source_block_index != loop->header_index ||
      exit->successor_index != 1 || !loom_cfg_cond_br_isa(terminator)) {
    return unknown;
  }
  const loom_value_id_t condition = loom_value_fact_table_query_identity(
      table, loom_cfg_cond_br_condition(terminator));
  const loom_op_t* compare =
      loom_value_fact_induction_defining_op(module, condition);
  if (compare && iree_any_bit_set(compare->traits, LOOM_TRAIT_CONSTANT_LIKE) &&
      loom_value_facts_is_zero(
          loom_value_fact_table_lookup(table, condition))) {
    return (loom_value_fact_induction_t){
        .value = LOOM_VALUE_ID_INVALID,
        .exits_at_header = true,
    };
  }
  if (!compare || !loom_index_cmp_isa(compare)) {
    return unknown;
  }
  loom_loop_bound_flags_t bound_flags = LOOM_LOOP_BOUND_NONE;
  if (!loom_value_fact_induction_compare_flags(compare, &bound_flags)) {
    return unknown;
  }
  const loom_value_id_t counter =
      loom_value_fact_table_query_identity(table, loom_index_cmp_lhs(compare));
  const loom_value_t* value = loom_module_value(module, counter);
  if (!loom_value_is_block_arg(value) ||
      loom_value_def_block(value) != header ||
      !loom_type_is_scalar(loom_module_value_type(module, counter))) {
    return unknown;
  }
  const uint16_t argument_index = loom_value_def_index(value);
  const loom_cfg_edge_info_t* entry = &graph->edges[loop->entries.unique_index];
  const loom_cfg_edge_info_t* backedge =
      &graph->edges[loop->backedges.unique_index];
  const loom_op_t* entry_branch =
      graph->blocks[entry->source_block_index].block->last_op;
  const loom_op_t* backedge_branch =
      graph->blocks[backedge->source_block_index].block->last_op;
  if (!loom_cfg_br_isa(entry_branch) || !loom_cfg_br_isa(backedge_branch)) {
    return unknown;
  }
  const loom_value_id_t upper_bound = loom_index_cmp_rhs(compare);
  const loom_value_id_t initial_value =
      loom_cfg_br_args(entry_branch).values[argument_index];
  if (!loom_value_fact_induction_is_invariant(
          module, loops, loop_index,
          loom_value_fact_table_query_identity(table, initial_value)) ||
      !loom_value_fact_induction_is_invariant(
          module, loops, loop_index,
          loom_value_fact_table_query_identity(table, upper_bound))) {
    return unknown;
  }
  loom_value_fact_induction_t induction = {
      .value = counter,
      .initial_value =
          loom_value_fact_recurrence_operand_make(table, module, initial_value),
      .upper_bound =
          loom_value_fact_recurrence_operand_make(table, module, upper_bound),
      .step = {.value = LOOM_VALUE_ID_INVALID},
      .bound_flags = bound_flags,
  };
  // A false entry guard proves an empty loop even when its dead increment has
  // folded away. Retain the guard equation independently of the backedge form.
  const loom_value_id_t next = loom_value_fact_table_query_identity(
      table, loom_cfg_br_args(backedge_branch).values[argument_index]);
  const loom_op_t* add = loom_value_fact_induction_defining_op(module, next);
  if (!add || !loom_index_add_isa(add)) {
    return induction;
  }
  const loom_value_id_t lhs =
      loom_value_fact_table_query_identity(table, loom_index_add_lhs(add));
  const loom_value_id_t rhs =
      loom_value_fact_table_query_identity(table, loom_index_add_rhs(add));
  const loom_value_id_t step =
      lhs == counter ? loom_index_add_rhs(add) : loom_index_add_lhs(add);
  if ((lhs == counter || rhs == counter) &&
      loom_value_fact_induction_is_invariant(
          module, loops, loop_index,
          loom_value_fact_table_query_identity(table, step))) {
    induction.step =
        loom_value_fact_recurrence_operand_make(table, module, step);
  }
  return induction;
}

// A value visible at this single-block boundary is either a local definition
// or a dominating capture. Literal constants remain invariant inside a region.
static bool loom_value_fact_loop_is_invariant(const loom_module_t* module,
                                              loom_loop_like_t loop,
                                              loom_value_id_t value_id) {
  const loom_value_t* value = loom_module_value(module, value_id);
  const loom_block_t* block = NULL;
  if (loom_value_is_block_arg(value)) {
    block = loom_value_def_block(value);
  } else {
    const loom_op_t* op = loom_value_def_op(value);
    if (loom_index_constant_isa(op)) {
      return true;
    }
    block = op->parent_block;
  }
  return block->parent_region != loom_loop_like_condition_region(loop) &&
         block->parent_region != loom_loop_like_body(loop);
}

static bool loom_value_fact_condition_forwards_counter(
    const loom_module_t* module, const loom_value_fact_table_t* table,
    const loom_op_t* condition, const loom_block_t* body,
    loom_value_id_t value_id, loom_value_id_t counter) {
  const loom_value_t* value = loom_module_value(module, value_id);
  return loom_value_is_block_arg(value) &&
         loom_value_def_block(value) == body &&
         loom_value_fact_table_query_identity(
             table, loom_op_const_operands(
                        condition)[1 + loom_value_def_index(value)]) == counter;
}

loom_value_fact_induction_t loom_value_fact_condition_loop_induction(
    const loom_value_fact_table_t* table, const loom_module_t* module,
    loom_loop_like_t loop) {
  const loom_value_fact_induction_t unknown = {
      .value = LOOM_VALUE_ID_INVALID,
  };
  const loom_block_t* before =
      loom_region_const_entry_block(loom_loop_like_condition_region(loop));
  const loom_block_t* body =
      loom_region_const_entry_block(loom_loop_like_body(loop));
  const loom_op_t* condition = before->last_op;
  const loom_op_t* yield = body->last_op;
  const loom_value_slice_t initial = loom_loop_like_iter_args(loop);
  const uint16_t result_count = loop.op->result_count;
  // Rewriter builders publish the shell before its terminators are complete.
  if (!condition || condition->operand_count != result_count + 1 || !yield ||
      yield->operand_count != initial.count) {
    return unknown;
  }
  const loom_value_id_t selector = loom_value_fact_table_query_identity(
      table, loom_op_const_operands(condition)[0]);
  const loom_op_t* compare =
      loom_value_fact_induction_defining_op(module, selector);
  if (compare && iree_any_bit_set(compare->traits, LOOM_TRAIT_CONSTANT_LIKE) &&
      loom_value_facts_is_zero(loom_value_fact_table_lookup(table, selector))) {
    return (loom_value_fact_induction_t){
        .value = LOOM_VALUE_ID_INVALID,
        .exits_at_header = true,
    };
  }
  loom_loop_bound_flags_t bound_flags = LOOM_LOOP_BOUND_NONE;
  if (!compare || !loom_index_cmp_isa(compare) ||
      !loom_value_fact_induction_compare_flags(compare, &bound_flags)) {
    return unknown;
  }
  const loom_value_id_t counter =
      loom_value_fact_table_query_identity(table, loom_index_cmp_lhs(compare));
  const loom_value_t* value = loom_module_value(module, counter);
  if (!loom_value_is_block_arg(value) ||
      loom_value_def_block(value) != before ||
      !loom_type_is_scalar(loom_module_value_type(module, counter))) {
    return unknown;
  }
  const uint16_t index = loom_value_def_index(value);
  const loom_value_id_t upper = loom_index_cmp_rhs(compare);
  if (!loom_value_fact_loop_is_invariant(
          module, loop, loom_value_fact_table_query_identity(table, upper))) {
    return unknown;
  }
  loom_value_fact_induction_t induction = {
      .value = counter,
      .initial_value = loom_value_fact_recurrence_operand_make(
          table, module, initial.values[index]),
      .upper_bound =
          loom_value_fact_recurrence_operand_make(table, module, upper),
      .step = {.value = LOOM_VALUE_ID_INVALID},
      .bound_flags = bound_flags,
  };
  const loom_value_id_t next = loom_value_fact_table_query_identity(
      table, loom_op_const_operands(yield)[index]);
  const loom_op_t* add = loom_value_fact_induction_defining_op(module, next);
  if (!add || !loom_index_add_isa(add)) {
    return induction;
  }
  const loom_value_id_t lhs =
      loom_value_fact_table_query_identity(table, loom_index_add_lhs(add));
  const loom_value_id_t rhs =
      loom_value_fact_table_query_identity(table, loom_index_add_rhs(add));
  loom_value_id_t step = LOOM_VALUE_ID_INVALID;
  if (loom_value_fact_condition_forwards_counter(module, table, condition, body,
                                                 lhs, counter)) {
    step = loom_index_add_rhs(add);
  } else if (loom_value_fact_condition_forwards_counter(
                 module, table, condition, body, rhs, counter)) {
    step = loom_index_add_lhs(add);
  }
  if (step != LOOM_VALUE_ID_INVALID &&
      loom_value_fact_loop_is_invariant(
          module, loop, loom_value_fact_table_query_identity(table, step))) {
    induction.step =
        loom_value_fact_recurrence_operand_make(table, module, step);
  }
  return induction;
}

typedef struct loom_value_fact_loop_scope_t {
  // Module defining the queried values.
  const loom_module_t* module;
  // Structured loop whose captures are invariant.
  loom_loop_like_t loop;
} loom_value_fact_loop_scope_t;

static bool loom_value_fact_loop_scope_is_invariant(void* user_data,
                                                    loom_value_id_t value) {
  const loom_value_fact_loop_scope_t* scope = user_data;
  return loom_value_fact_loop_is_invariant(scope->module, scope->loop, value);
}

iree_status_t loom_value_fact_loop_build_recurrences(
    const loom_value_fact_table_t* table, const loom_module_t* module,
    loom_loop_like_t loop, iree_arena_allocator_t* arena,
    loom_value_fact_recurrence_set_t* out_set) {
  *out_set = (loom_value_fact_recurrence_set_t){0};
  const loom_block_t* body =
      loom_region_const_entry_block(loom_loop_like_body(loop));
  const loom_region_t* condition_region = loom_loop_like_condition_region(loop);
  const loom_block_t* header =
      condition_region ? loom_region_const_entry_block(condition_region) : body;
  const loom_value_slice_t initial = loom_loop_like_iter_args(loop);
  loom_op_t* yield = body->last_op;
  loom_op_t* condition = condition_region ? header->last_op : NULL;
  // A rewriter can publish a loop shell before completing its terminators.
  if (!yield || yield->operand_count != initial.count ||
      (condition_region &&
       (!condition || condition->operand_count != body->arg_count + 1))) {
    return iree_ok_status();
  }
  loom_value_fact_loop_scope_t owner = {.module = module, .loop = loop};
  const loom_value_fact_recurrence_scope_t scope = {
      .user_data = &owner,
      .is_invariant = loom_value_fact_loop_scope_is_invariant,
      .forwarding_block = condition ? body : NULL,
      .forwarding_values =
          condition ? (loom_value_slice_t){loom_op_operands(condition) + 1,
                                           body->arg_count}
                    : (loom_value_slice_t){0},
  };
  const uint16_t argument_offset =
      loop.vtable->iv_block_arg_index == LOOM_BLOCK_ARG_INDEX_NONE
          ? 0
          : (uint16_t)loop.vtable->iv_block_arg_index + 1;
  return loom_value_fact_recurrence_set_build(
      table, module, header, argument_offset, initial,
      (loom_value_slice_t){loom_op_operands(yield), initial.count}, &scope,
      arena, out_set);
}

typedef struct loom_value_fact_cfg_loop_scope_t {
  // Module defining the queried values.
  const loom_module_t* module;
  // Natural-loop membership established by the CFG owner.
  const loom_cfg_loop_nest_t* loops;
  // Loop whose captures are invariant.
  uint16_t loop_index;
} loom_value_fact_cfg_loop_scope_t;

static bool loom_value_fact_cfg_loop_scope_is_invariant(void* user_data,
                                                        loom_value_id_t value) {
  const loom_value_fact_cfg_loop_scope_t* scope = user_data;
  return loom_value_fact_induction_is_invariant(scope->module, scope->loops,
                                                scope->loop_index, value);
}

iree_status_t loom_value_fact_cfg_build_recurrences(
    const loom_value_fact_table_t* table, const loom_module_t* module,
    const loom_value_fact_cfg_region_t* region, uint16_t block_index,
    iree_arena_allocator_t* arena) {
  const uint16_t loop_index =
      loom_cfg_loop_nest_innermost(&region->loops, block_index);
  if (loop_index == LOOM_CFG_LOOP_NEST_NONE ||
      region->loops.loops[loop_index].header_index != block_index) {
    return iree_ok_status();
  }
  loom_value_fact_recurrence_set_t* set = &region->recurrences[loop_index];
  *set = (loom_value_fact_recurrence_set_t){0};
  region->inductions[loop_index] = loom_value_fact_induction_recognize(
      table, module, &region->loops, loop_index);
  if (region->inductions[loop_index].value == LOOM_VALUE_ID_INVALID) {
    return iree_ok_status();
  }
  const loom_cfg_natural_loop_t* loop = &region->loops.loops[loop_index];
  const loom_cfg_edge_info_t* entry =
      &region->graph.edges[loop->entries.unique_index];
  const loom_cfg_edge_info_t* backedge =
      &region->graph.edges[loop->backedges.unique_index];
  loom_value_fact_cfg_loop_scope_t owner = {
      .module = module,
      .loops = &region->loops,
      .loop_index = loop_index,
  };
  const loom_value_fact_recurrence_scope_t scope = {
      .user_data = &owner,
      .is_invariant = loom_value_fact_cfg_loop_scope_is_invariant,
  };
  return loom_value_fact_recurrence_set_build(
      table, module, region->graph.blocks[block_index].block, 0,
      loom_cfg_br_args(
          region->graph.blocks[entry->source_block_index].block->last_op),
      loom_cfg_br_args(
          region->graph.blocks[backedge->source_block_index].block->last_op),
      &scope, arena, set);
}

void loom_value_fact_cfg_update_induction(
    const loom_value_fact_table_t* table, const loom_module_t* module,
    const loom_value_fact_cfg_region_t* region, uint16_t block_index) {
  const uint16_t loop_index =
      loom_cfg_loop_nest_innermost(&region->loops, block_index);
  if (loop_index == LOOM_CFG_LOOP_NEST_NONE ||
      region->loops.loops[loop_index].header_index != block_index) {
    return;
  }
  region->inductions[loop_index] = loom_value_fact_induction_recognize(
      table, module, &region->loops, loop_index);
}

loom_loop_recurrence_facts_t loom_value_fact_induction_facts(
    const loom_value_fact_table_t* table, const loom_module_t* module,
    const loom_value_fact_induction_t* induction) {
  const loom_loop_recurrence_facts_t unknown = {
      .values = loom_value_facts_unknown(),
      .body_values = loom_value_facts_unknown(),
      .exit_value = loom_value_facts_unknown(),
  };
  if (induction->exits_at_header) {
    return (loom_loop_recurrence_facts_t){
        .values = loom_value_facts_unknown(),
        .body_values = loom_value_facts_unknown(),
        .exit_value = loom_value_facts_unknown(),
        .trip_count_known = true,
    };
  }
  if (induction->value == LOOM_VALUE_ID_INVALID) {
    return unknown;
  }
  const loom_value_facts_t initial =
      loom_value_fact_recurrence_operand_facts(table, induction->initial_value);
  const loom_value_facts_t upper =
      loom_value_fact_recurrence_operand_facts(table, induction->upper_bound);
  const loom_value_facts_t step =
      loom_value_fact_recurrence_operand_facts(table, induction->step);
  if (!loom_value_facts_is_exact(initial) ||
      !loom_value_facts_is_exact(upper)) {
    return unknown;
  }
  const loom_scalar_type_t scalar_type =
      loom_type_element_type(loom_module_value_type(module, induction->value));
  uint8_t bitwidth = 64;
  if (table->context.target_facts) {
    const loom_target_snapshot_t* target =
        &table->context.target_facts->storage.snapshot;
    bitwidth = scalar_type == LOOM_SCALAR_TYPE_INDEX ? target->index_bitwidth
                                                     : target->offset_bitwidth;
  }
  return loom_loop_domain_recurrence_facts(
      induction->bound_flags, bitwidth, initial.range_lo, upper.range_lo,
      loom_value_facts_is_exact(step) ? step.range_lo : 0);
}
