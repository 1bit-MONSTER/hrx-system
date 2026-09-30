// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/analysis/conditioned_value_facts.h"

#include <string.h>

#include "loom/analysis/cfg_value_identity.h"
#include "loom/analysis/condition_fact_scope.h"
#include "loom/ops/cfg/ops.h"
#include "loom/ops/op_defs.h"

typedef struct loom_conditioned_value_facts_region_t {
  // Immutable region described by this record.
  const loom_region_t* region;
  // Borrowed fact-owner structure, or NULL for a structured region.
  const loom_value_fact_cfg_region_t* structure;
  // Lexical scope per block, indexed by its dense region ordinal.
  loom_condition_fact_scope_t* scopes;
  // Next record in the region-address hash bucket.
  struct loom_conditioned_value_facts_region_t* next_bucket;
  // Next record in the complete scope list.
  struct loom_conditioned_value_facts_region_t* next;
} loom_conditioned_value_facts_region_t;

typedef struct loom_conditioned_value_facts_local_scope_t {
  // Complete Boolean-edge derivation owned by this lexical scope.
  loom_condition_derivation_t derivation;
  // Scope node referencing |derivation| and its outer lexical scope.
  loom_condition_fact_scope_t scope;
  // Next local scope whose value identities require canonicalization.
  struct loom_conditioned_value_facts_local_scope_t* next;
} loom_conditioned_value_facts_local_scope_t;

typedef struct loom_conditioned_value_facts_t {
  // Function whose IR stays immutable throughout the solve.
  loom_module_t* module;
  // Numeric scope being refined in place.
  loom_value_fact_table_t* table;
  // Construction and retained relation storage for this solve.
  iree_arena_allocator_t arena;
  // Temporary function-local value identity domain.
  loom_local_value_domain_t domain;
  // Exact CFG forwarding identities, independent of numeric ranges.
  loom_cfg_value_identity_table_t identities;
  // Reusable recursive condition query over this immutable function.
  loom_condition_query_t query;
  // Dominance wrapper borrowing the fact owner's existing trees.
  loom_dominance_info_t dominance;
  // Region-address hash buckets for per-operation scope lookup.
  loom_conditioned_value_facts_region_t** buckets;
  // Power-of-two number of buckets.
  iree_host_size_t bucket_count;
  // All scope records, with arena lifetime.
  loom_conditioned_value_facts_region_t* regions;
  // Local Boolean-edge scopes retained during the solve.
  loom_conditioned_value_facts_local_scope_t* local_scopes;
} loom_conditioned_value_facts_t;

static iree_host_size_t loom_conditioned_value_facts_bucket(
    const loom_conditioned_value_facts_t* state, const loom_region_t* region) {
  uintptr_t bits = (uintptr_t)region;
  bits ^= bits >> 17;
  bits *= (uintptr_t)0xed5ad4bbU;
  bits ^= bits >> 11;
  return (iree_host_size_t)bits & (state->bucket_count - 1);
}

static iree_status_t loom_conditioned_value_facts_extend_region(
    loom_conditioned_value_facts_t* state, loom_op_t* parent_op,
    loom_region_t* region, const loom_condition_fact_scope_t* parent,
    const loom_condition_fact_scope_t** out_scope) {
  IREE_RETURN_IF_ERROR(loom_condition_fact_scope_extend_region(
      state->table, region, parent, &state->arena, out_scope));
  const loom_region_branch_truth_t truth =
      loom_value_fact_table_lookup_region_branch_truth(state->table, region);
  if (truth == LOOM_REGION_BRANCH_TRUTH_UNKNOWN) {
    return iree_ok_status();
  }

  loom_region_branch_t branch =
      loom_region_branch_cast(state->module, parent_op);
  IREE_ASSERT(loom_region_branch_isa(branch));
  loom_condition_derivation_t derivation;
  loom_condition_derivation_initialize(&state->arena, &derivation);
  IREE_RETURN_IF_ERROR(loom_condition_facts_query_complete(
      &state->query, state->table, loom_region_branch_selector(branch),
      truth == LOOM_REGION_BRANCH_TRUTH_TRUE, &derivation));
  if (!derivation.integer_facts.integer_relation_count) {
    return iree_ok_status();
  }
  loom_conditioned_value_facts_local_scope_t* local_scope = NULL;
  IREE_RETURN_IF_ERROR(iree_arena_allocate(&state->arena, sizeof(*local_scope),
                                           (void**)&local_scope));
  local_scope->derivation = derivation;
  loom_condition_fact_scope_initialize_local(
      *out_scope, &local_scope->derivation, &local_scope->scope);
  local_scope->next = state->local_scopes;
  state->local_scopes = local_scope;
  *out_scope = &local_scope->scope;
  return iree_ok_status();
}

static iree_status_t loom_conditioned_value_facts_collect(
    loom_conditioned_value_facts_t* state, loom_region_t* region,
    const loom_condition_fact_scope_t* parent) {
  loom_conditioned_value_facts_region_t* entry = NULL;
  IREE_RETURN_IF_ERROR(
      iree_arena_allocate(&state->arena, sizeof(*entry), (void**)&entry));
  const iree_host_size_t bucket =
      loom_conditioned_value_facts_bucket(state, region);
  *entry = (loom_conditioned_value_facts_region_t){
      .region = region,
      .structure =
          loom_value_fact_table_lookup_cfg_region(state->table, region),
      .next_bucket = state->buckets[bucket],
      .next = state->regions,
  };
  state->buckets[bucket] = entry;
  state->regions = entry;
  if (entry->structure) {
    IREE_RETURN_IF_ERROR(loom_dominance_info_add_cfg_graph(
        &state->dominance, &entry->structure->graph,
        &entry->structure->dominance));
  }
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      &state->arena, region->block_count, sizeof(*entry->scopes),
      (void**)&entry->scopes));
  loom_block_t* block = NULL;
  loom_region_for_each_block(region, block) {
    loom_condition_fact_scope_t* scope = &entry->scopes[block->region_index];
    *scope = (loom_condition_fact_scope_t){.parent = parent};
    loom_op_t* op = NULL;
    loom_block_for_each_op(block, op) {
      loom_region_t** regions = loom_op_regions(op);
      for (uint8_t i = 0; i < op->region_count; ++i) {
        if (regions[i]) {
          const loom_condition_fact_scope_t* child_scope = NULL;
          IREE_RETURN_IF_ERROR(loom_conditioned_value_facts_extend_region(
              state, op, regions[i], scope, &child_scope));
          IREE_RETURN_IF_ERROR(loom_conditioned_value_facts_collect(
              state, regions[i], child_scope));
        }
      }
    }
  }
  return iree_ok_status();
}

static void loom_conditioned_value_facts_refine_operands(
    void* user_data, const loom_value_fact_table_t* table, const loom_op_t* op,
    loom_value_facts_t* operands) {
  if (!op->operand_count) {
    return;
  }
  const loom_conditioned_value_facts_t* state = user_data;
  const loom_region_t* region = op->parent_block->parent_region;
  const iree_host_size_t bucket =
      loom_conditioned_value_facts_bucket(state, region);
  const loom_conditioned_value_facts_region_t* entry = state->buckets[bucket];
  while (entry && entry->region != region) {
    entry = entry->next_bucket;
  }
  // Projected configuration regions lie outside the function body domain.
  if (!entry) {
    return;
  }
  const loom_condition_fact_scope_t* scope =
      &entry->scopes[op->parent_block->region_index];
  const loom_value_id_t* values = loom_op_const_operands(op);
  for (uint16_t i = 0; i < op->operand_count; ++i) {
    const loom_value_id_t canonical =
        loom_cfg_value_identity_table_lookup(&state->identities, values[i]);
    loom_condition_fact_scope_apply_to_value_facts(scope, table, canonical,
                                                   &operands[i]);
  }
}

static void loom_conditioned_value_facts_canonicalize_derivation(
    const loom_cfg_value_identity_table_t* identities,
    loom_condition_derivation_t* derivation) {
  for (iree_host_size_t i = 0;
       i < derivation->integer_facts.integer_relation_count; ++i) {
    loom_condition_integer_relation_t* relation =
        &derivation->integer_facts.integer_relations[i];
    if (relation->left.kind == LOOM_CONDITION_INTEGER_OPERAND_VALUE) {
      relation->left.value_id = loom_cfg_value_identity_table_lookup(
          identities, relation->left.value_id);
    }
    if (relation->right.kind == LOOM_CONDITION_INTEGER_OPERAND_VALUE) {
      relation->right.value_id = loom_cfg_value_identity_table_lookup(
          identities, relation->right.value_id);
    }
  }
}

static iree_status_t loom_conditioned_value_facts_solve(
    loom_conditioned_value_facts_t* state, loom_func_like_t function) {
  state->dominance = (loom_dominance_info_t){
      .module = state->module,
      .arena = &state->arena,
  };
  state->bucket_count = state->table->regions.bucket_count;
  loom_condition_query_initialize(state->module, &state->domain, &state->arena,
                                  &state->query);
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      &state->arena, state->bucket_count, sizeof(*state->buckets),
      (void**)&state->buckets));
  memset(state->buckets, 0, state->bucket_count * sizeof(*state->buckets));
  IREE_RETURN_IF_ERROR(loom_conditioned_value_facts_collect(
      state, loom_func_like_body(function), NULL));
  IREE_RETURN_IF_ERROR(loom_cfg_value_identity_table_initialize(
      &state->domain, &state->arena, &state->identities));
  for (loom_conditioned_value_facts_region_t* entry = state->regions; entry;
       entry = entry->next) {
    if (entry->structure) {
      IREE_RETURN_IF_ERROR(loom_cfg_value_identity_table_update(
          &state->identities, entry->structure, &state->dominance,
          &state->arena));
    }
  }
  for (loom_conditioned_value_facts_local_scope_t* local_scope =
           state->local_scopes;
       local_scope; local_scope = local_scope->next) {
    loom_conditioned_value_facts_canonicalize_derivation(
        &state->identities, &local_scope->derivation);
  }
  bool has_conditions = state->table->condition_integer_projection_count != 0 ||
                        state->local_scopes != NULL;
  for (loom_conditioned_value_facts_region_t* entry = state->regions; entry;
       entry = entry->next) {
    if (!entry->structure) {
      continue;
    }
    const loom_cfg_graph_t* graph = &entry->structure->graph;
    const loom_cfg_dominance_t* dominance = &entry->structure->dominance;
    for (iree_host_size_t i = 1; i < dominance->preorder.count; ++i) {
      const uint16_t block = dominance->preorder.values[i];
      const uint16_t parent = dominance->immediate_dominators[block];
      loom_condition_fact_scope_t* scope = &entry->scopes[block];
      const loom_condition_fact_scope_t* parent_scope = &entry->scopes[parent];
      scope->parent =
          parent_scope->local_derivation ? parent_scope : parent_scope->parent;
      const uint16_t predecessor = dominance->entry_predecessors[block];
      if (predecessor == LOOM_CFG_DOMINATOR_INVALID) {
        continue;
      }
      // The dominance owner has already proved that this predecessor's direct
      // alternative enters the block and dominates its entire subtree.
      const loom_cfg_edge_index_span_t edges =
          loom_cfg_graph_successor_edges(graph, predecessor);
      const loom_cfg_edge_info_t* first = &graph->edges[edges.values[0]];
      if (!loom_cfg_cond_br_isa(first->terminator)) {
        continue;
      }
      const loom_cfg_edge_info_t* second = &graph->edges[edges.values[1]];
      // Parallel outcomes establish no truth value for their common target.
      if (first->target_block_index == second->target_block_index) {
        continue;
      }
      const loom_cfg_edge_info_t* edge =
          first->target_block_index == block ? first : second;
      loom_condition_derivation_t derivation;
      loom_condition_derivation_initialize(&state->arena, &derivation);
      IREE_RETURN_IF_ERROR(loom_condition_facts_query_complete(
          &state->query, state->table,
          loom_cfg_cond_br_condition(edge->terminator),
          edge->successor_index == 0, &derivation));
      loom_conditioned_value_facts_canonicalize_derivation(&state->identities,
                                                           &derivation);
      if (derivation.integer_facts.integer_relation_count) {
        loom_condition_derivation_t* retained_derivation = NULL;
        IREE_RETURN_IF_ERROR(iree_arena_allocate(&state->arena,
                                                 sizeof(*retained_derivation),
                                                 (void**)&retained_derivation));
        *retained_derivation = derivation;
        scope->local_derivation = retained_derivation;
        has_conditions = true;
      }
    }
  }
  if (!has_conditions) {
    return iree_ok_status();
  }
  state->table->has_conditioned_results = true;
  state->table->context.refine_operands.user_data = state;
  state->table->context.refine_operands.fn =
      loom_conditioned_value_facts_refine_operands;
  iree_status_t status =
      loom_value_fact_table_compute(state->table, state->module, function);
  state->table->context.refine_operands.fn = NULL;
  state->table->context.refine_operands.user_data = NULL;
  return status;
}

iree_status_t loom_conditioned_value_facts_compute(
    loom_value_fact_table_t* table, loom_module_t* module,
    loom_func_like_t function) {
  if ((!table->regions.cfg_count &&
       !table->condition_integer_projection_count &&
       !table->has_boolean_branch_regions) ||
      !loom_func_like_body(function)) {
    return iree_ok_status();
  }
  loom_conditioned_value_facts_t state = {.module = module, .table = table};
  iree_arena_initialize(table->arena->block_pool, &state.arena);
  iree_status_t status = loom_local_value_domain_acquire_for_region_tree(
      module, loom_func_like_body(function), &state.arena, &state.domain);
  if (iree_status_is_ok(status)) {
    status = loom_conditioned_value_facts_solve(&state, function);
  }
  if (loom_local_value_domain_is_acquired(&state.domain)) {
    loom_local_value_domain_release(&state.domain);
  }
  iree_arena_deinitialize(&state.arena);
  return status;
}
