// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/transforms/cleanup/integer_division.h"

#include <string.h>

#include "loom/ir/module.h"
#include "loom/ir/structural_hash.h"
#include "loom/ops/index/ops.h"
#include "loom/ops/scalar/ops.h"
#include "loom/ops/vector/ops.h"
#include "loom/rewrite/rewriter.h"
#include "loom/util/dominance.h"
#include "loom/util/walk.h"

typedef enum loom_integer_division_family_e {
  LOOM_INTEGER_DIVISION_FAMILY_SCALAR_SIGNED = 0,
  LOOM_INTEGER_DIVISION_FAMILY_SCALAR_UNSIGNED = 1,
  LOOM_INTEGER_DIVISION_FAMILY_INDEX = 2,
  LOOM_INTEGER_DIVISION_FAMILY_VECTOR_SIGNED = 3,
  LOOM_INTEGER_DIVISION_FAMILY_VECTOR_UNSIGNED = 4,
} loom_integer_division_family_t;

typedef enum loom_integer_division_role_e {
  LOOM_INTEGER_DIVISION_ROLE_QUOTIENT = 0,
  LOOM_INTEGER_DIVISION_ROLE_REMAINDER = 1,
} loom_integer_division_role_t;

typedef struct loom_integer_division_key_t {
  // Retained identity of the dividend.
  loom_value_id_t lhs;
  // Retained identity of the divisor.
  loom_value_id_t rhs;
  // Result type shared by both operations.
  loom_type_t type;
  // Scalar/index/vector and signedness family.
  loom_integer_division_family_t family;
  // Structural hash of all key fields.
  uint32_t hash;
} loom_integer_division_key_t;

typedef struct loom_integer_division_plan_t loom_integer_division_plan_t;

typedef struct loom_integer_division_candidate_t {
  // Standalone quotient or remainder operation.
  loom_op_t* op;
  // The operation's only result.
  loom_value_id_t result;
  // Semantic result selected by the operation.
  loom_integer_division_role_t role;
  // Earlier dominating candidate with the same role and key.
  struct loom_integer_division_candidate_t* previous_same_role;
  // Selected combined plan, or NULL while unmatched.
  loom_integer_division_plan_t* plan;
  // Next candidate retired by the selected plan.
  struct loom_integer_division_candidate_t* plan_next;
} loom_integer_division_candidate_t;

struct loom_integer_division_plan_t {
  // Next plan across all function regions.
  loom_integer_division_plan_t* next;
  // Operation before which the combined operation is inserted.
  loom_op_t* anchor;
  // First standalone operation retired by this plan.
  loom_integer_division_candidate_t* first_candidate;
  // Last standalone operation retired by this plan.
  loom_integer_division_candidate_t* last_candidate;
  // Preferred source name for the quotient result.
  loom_value_id_t quotient_name_source;
  // Preferred source name for the remainder result.
  loom_value_id_t remainder_name_source;
  // Combined operation family.
  loom_integer_division_family_t family;
  // Number of standalone operations retired by this plan.
  uint32_t candidate_count;
};

typedef struct loom_integer_division_binding_t {
  // Normalized operand and result identity.
  loom_integer_division_key_t key;
  // Most recent candidate visible for the key.
  loom_integer_division_candidate_t* candidate;
  // Previous visible binding for this key.
  uint32_t previous;
  // Distance from key.hash to the occupied bucket.
  uint32_t probe_distance;
} loom_integer_division_binding_t;

typedef struct loom_integer_division_scope_t {
  // Enclosing dominance scope.
  struct loom_integer_division_scope_t* parent;
  // Binding count restored when leaving this scope.
  uint32_t binding_start;
  // Earliest binding visible across isolation boundaries.
  uint32_t visible_start;
} loom_integer_division_scope_t;

typedef struct loom_integer_division_inventory_t {
  // Retained canonical value facts used to exclude exact divisors.
  const loom_value_fact_table_t* facts;
  // Upper bound on candidate and binding records.
  uint32_t candidate_count;
  // Exact number of block scopes in the region tree.
  iree_host_size_t scope_count;
} loom_integer_division_inventory_t;

typedef struct loom_integer_division_planner_t {
  // Module containing the immutable function snapshot.
  loom_module_t* module;
  // Retained canonical value facts.
  const loom_value_fact_table_t* facts;
  // Arena retaining plans and candidates through application.
  iree_arena_allocator_t* plan_arena;
  // First selected plan across function regions.
  loom_integer_division_plan_t* first_plan;
  // Last selected plan across function regions.
  loom_integer_division_plan_t* last_plan;
} loom_integer_division_planner_t;

typedef struct loom_integer_division_region_planner_t {
  // Function-wide plan owner.
  loom_integer_division_planner_t* planner;
  // Shared dominator-order walk.
  loom_dominance_walk_t* dominance_walk;
  // Innermost active rollback scope.
  loom_integer_division_scope_t* active_scope;
  // Fixed scope inventory owned by this region plan.
  loom_integer_division_scope_t* scopes;
  // Number of entered scopes.
  iree_host_size_t scope_count;
  // Candidate records owned by this region plan.
  loom_integer_division_candidate_t* candidates;
  // Number of populated candidate records.
  uint32_t candidate_count;
  // Selected plan storage owned by this region plan.
  loom_integer_division_plan_t* plans;
  // Number of populated plans.
  uint32_t plan_count;
  // Scoped rollback bindings.
  loom_integer_division_binding_t* bindings;
  // Latest binding index for each open-addressed key.
  uint32_t* bucket_heads;
  // Power-of-two bucket count minus one.
  iree_host_size_t bucket_mask;
  // Number of active bindings.
  uint32_t binding_count;
} loom_integer_division_region_planner_t;

static bool loom_integer_division_match(
    const loom_op_t* op, loom_integer_division_family_t* out_family,
    loom_integer_division_role_t* out_role) {
  switch (op->kind) {
    case LOOM_OP_SCALAR_DIVSI:
      *out_family = LOOM_INTEGER_DIVISION_FAMILY_SCALAR_SIGNED;
      *out_role = LOOM_INTEGER_DIVISION_ROLE_QUOTIENT;
      return true;
    case LOOM_OP_SCALAR_REMSI:
      *out_family = LOOM_INTEGER_DIVISION_FAMILY_SCALAR_SIGNED;
      *out_role = LOOM_INTEGER_DIVISION_ROLE_REMAINDER;
      return true;
    case LOOM_OP_SCALAR_DIVUI:
      *out_family = LOOM_INTEGER_DIVISION_FAMILY_SCALAR_UNSIGNED;
      *out_role = LOOM_INTEGER_DIVISION_ROLE_QUOTIENT;
      return true;
    case LOOM_OP_SCALAR_REMUI:
      *out_family = LOOM_INTEGER_DIVISION_FAMILY_SCALAR_UNSIGNED;
      *out_role = LOOM_INTEGER_DIVISION_ROLE_REMAINDER;
      return true;
    case LOOM_OP_INDEX_DIV:
      *out_family = LOOM_INTEGER_DIVISION_FAMILY_INDEX;
      *out_role = LOOM_INTEGER_DIVISION_ROLE_QUOTIENT;
      return true;
    case LOOM_OP_INDEX_REM:
      *out_family = LOOM_INTEGER_DIVISION_FAMILY_INDEX;
      *out_role = LOOM_INTEGER_DIVISION_ROLE_REMAINDER;
      return true;
    case LOOM_OP_VECTOR_DIVSI:
      *out_family = LOOM_INTEGER_DIVISION_FAMILY_VECTOR_SIGNED;
      *out_role = LOOM_INTEGER_DIVISION_ROLE_QUOTIENT;
      return true;
    case LOOM_OP_VECTOR_REMSI:
      *out_family = LOOM_INTEGER_DIVISION_FAMILY_VECTOR_SIGNED;
      *out_role = LOOM_INTEGER_DIVISION_ROLE_REMAINDER;
      return true;
    case LOOM_OP_VECTOR_DIVUI:
      *out_family = LOOM_INTEGER_DIVISION_FAMILY_VECTOR_UNSIGNED;
      *out_role = LOOM_INTEGER_DIVISION_ROLE_QUOTIENT;
      return true;
    case LOOM_OP_VECTOR_REMUI:
      *out_family = LOOM_INTEGER_DIVISION_FAMILY_VECTOR_UNSIGNED;
      *out_role = LOOM_INTEGER_DIVISION_ROLE_REMAINDER;
      return true;
    default:
      return false;
  }
}

static bool loom_integer_division_family_is_signed(
    loom_integer_division_family_t family) {
  return family == LOOM_INTEGER_DIVISION_FAMILY_SCALAR_SIGNED ||
         family == LOOM_INTEGER_DIVISION_FAMILY_VECTOR_SIGNED;
}

// Uses the retained function-fact index to reject scopes that cannot contain a
// quotient/remainder pair. This keeps ordinary functions off the structural
// walker and dominance planner without reconstructing the analyzed scope.
static bool loom_integer_division_has_pair_candidates(
    const loom_module_t* module, const loom_value_fact_table_t* facts) {
  const loom_value_id_t* scope_values = NULL;
  iree_host_size_t scope_value_count = 0;
  loom_value_fact_table_scope_values(facts, &scope_values, &scope_value_count);
  uint32_t candidate_count = 0;
  for (iree_host_size_t i = 0; i < scope_value_count; ++i) {
    const loom_value_t* value = loom_module_value(module, scope_values[i]);
    if (loom_value_is_block_arg(value)) {
      continue;
    }
    const loom_op_t* op = loom_value_def_op(value);
    if (!op || iree_any_bit_set(op->flags, LOOM_OP_FLAG_DEAD)) {
      continue;
    }
    loom_integer_division_family_t family;
    loom_integer_division_role_t role;
    if (!loom_integer_division_match(op, &family, &role)) {
      continue;
    }
    const loom_value_id_t divisor = loom_op_const_operands(op)[1];
    if (loom_value_facts_is_exact(
            loom_value_fact_table_lookup(facts, divisor))) {
      continue;
    }
    if (++candidate_count == 2) {
      return true;
    }
  }
  return false;
}

static iree_status_t loom_integer_division_inventory_op(
    void* user_data, loom_op_t* op, const loom_walk_context_t* context,
    loom_walk_result_t* out_result) {
  (void)context;
  *out_result = LOOM_WALK_CONTINUE;
  loom_integer_division_inventory_t* inventory =
      (loom_integer_division_inventory_t*)user_data;
  loom_integer_division_family_t family;
  loom_integer_division_role_t role;
  if (loom_integer_division_match(op, &family, &role)) {
    const loom_value_id_t divisor = loom_op_const_operands(op)[1];
    if (!loom_value_facts_is_exact(
            loom_value_fact_table_lookup(inventory->facts, divisor))) {
      ++inventory->candidate_count;
    }
  }
  for (uint8_t i = 0; i < op->region_count; ++i) {
    const loom_region_t* region = loom_op_regions(op)[i];
    inventory->scope_count += region ? region->block_count : 0;
  }
  return iree_ok_status();
}

static iree_status_t loom_integer_division_inventory_region(
    const loom_module_t* module, const loom_value_fact_table_t* facts,
    loom_region_t* region, loom_integer_division_inventory_t* out_inventory) {
  *out_inventory = (loom_integer_division_inventory_t){
      .facts = facts,
      .scope_count = region->block_count,
  };
  loom_walk_result_t walk_result = LOOM_WALK_CONTINUE;
  return loom_walk_region(
      module, region, LOOM_WALK_PRE_ORDER,
      (loom_walk_callback_t){loom_integer_division_inventory_op, out_inventory},
      &walk_result);
}

static uint32_t loom_integer_division_hash_key(
    const loom_integer_division_key_t* key) {
  uint32_t hash = loom_structural_hash_initialize();
  hash = loom_structural_hash_mix_u8(hash, (uint8_t)key->family);
  hash = loom_structural_hash_mix_u32(hash, key->lhs);
  hash = loom_structural_hash_mix_u32(hash, key->rhs);
  hash = loom_structural_hash_mix_u32(hash, loom_type_hash(key->type));
  return loom_structural_hash_finalize(hash);
}

static bool loom_integer_division_key_equal(
    const loom_integer_division_key_t* lhs,
    const loom_integer_division_key_t* rhs) {
  return lhs->family == rhs->family && lhs->lhs == rhs->lhs &&
         lhs->rhs == rhs->rhs && loom_type_equal(lhs->type, rhs->type);
}

static iree_host_size_t loom_integer_division_find_slot(
    const loom_integer_division_region_planner_t* planner,
    const loom_integer_division_key_t* key) {
  iree_host_size_t slot = key->hash & planner->bucket_mask;
  while (planner->bucket_heads[slot] != UINT32_MAX) {
    const loom_integer_division_binding_t* binding =
        &planner->bindings[planner->bucket_heads[slot]];
    if (binding->key.hash == key->hash &&
        loom_integer_division_key_equal(&binding->key, key)) {
      break;
    }
    slot = (slot + 1) & planner->bucket_mask;
  }
  return slot;
}

static loom_integer_division_candidate_t* loom_integer_division_find(
    const loom_integer_division_region_planner_t* planner,
    const loom_integer_division_key_t* key) {
  const uint32_t index =
      planner->bucket_heads[loom_integer_division_find_slot(planner, key)];
  return index != UINT32_MAX && index >= planner->active_scope->visible_start
             ? planner->bindings[index].candidate
             : NULL;
}

static void loom_integer_division_insert(
    loom_integer_division_region_planner_t* planner,
    loom_integer_division_key_t key,
    loom_integer_division_candidate_t* candidate) {
  const iree_host_size_t slot = loom_integer_division_find_slot(planner, &key);
  const uint32_t previous = planner->bucket_heads[slot];
  const uint32_t index = planner->binding_count++;
  planner->bindings[index] = (loom_integer_division_binding_t){
      .key = key,
      .candidate = candidate,
      .previous = previous,
      .probe_distance = (uint32_t)((slot - key.hash) & planner->bucket_mask),
  };
  planner->bucket_heads[slot] = index;
}

static iree_status_t loom_integer_division_enter_scope(
    void* user_data, loom_dominance_walk_scope_flags_t flags) {
  loom_integer_division_region_planner_t* planner =
      (loom_integer_division_region_planner_t*)user_data;
  loom_integer_division_scope_t* parent = planner->active_scope;
  loom_integer_division_scope_t* scope =
      &planner->scopes[planner->scope_count++];
  const bool isolated = iree_any_bit_set(
      flags, LOOM_DOMINANCE_WALK_SCOPE_FLAG_ISOLATED |
                 LOOM_DOMINANCE_WALK_SCOPE_FLAG_INDEPENDENT_EXECUTION);
  *scope = (loom_integer_division_scope_t){
      .parent = parent,
      .binding_start = planner->binding_count,
      .visible_start =
          !parent || isolated ? planner->binding_count : parent->visible_start,
  };
  planner->active_scope = scope;
  return iree_ok_status();
}

static void loom_integer_division_leave_scope(void* user_data) {
  loom_integer_division_region_planner_t* planner =
      (loom_integer_division_region_planner_t*)user_data;
  const uint32_t checkpoint = planner->active_scope->binding_start;
  while (planner->binding_count > checkpoint) {
    const loom_integer_division_binding_t* binding =
        &planner->bindings[--planner->binding_count];
    const iree_host_size_t slot =
        ((iree_host_size_t)binding->key.hash + binding->probe_distance) &
        planner->bucket_mask;
    planner->bucket_heads[slot] = binding->previous;
  }
  planner->active_scope = planner->active_scope->parent;
}

static loom_integer_division_candidate_t*
loom_integer_division_oldest_candidate(
    loom_integer_division_candidate_t* candidate) {
  while (candidate->previous_same_role) {
    candidate = candidate->previous_same_role;
  }
  return candidate;
}

static bool loom_integer_division_candidate_uses_dominated(
    const loom_integer_division_region_planner_t* planner,
    const loom_integer_division_candidate_t* candidate,
    const loom_op_t* anchor) {
  const loom_value_t* value =
      loom_module_value(planner->planner->module, candidate->result);
  if (loom_value_has_attribute_uses(value) ||
      loom_module_value_has_type_uses(planner->planner->module,
                                      candidate->result)) {
    return false;
  }
  const loom_dominance_info_t* dominance =
      loom_dominance_walk_info(planner->dominance_walk);
  const loom_use_t* use = NULL;
  loom_value_for_each_use(value, use) {
    if (!loom_dominates_op(dominance, anchor, loom_use_user_op(*use))) {
      return false;
    }
  }
  return true;
}

static bool loom_integer_division_chain_uses_dominated(
    const loom_integer_division_region_planner_t* planner,
    const loom_integer_division_candidate_t* candidate,
    const loom_op_t* anchor) {
  for (; candidate; candidate = candidate->previous_same_role) {
    if (!loom_integer_division_candidate_uses_dominated(planner, candidate,
                                                        anchor)) {
      return false;
    }
  }
  return true;
}

static void loom_integer_division_plan_append(
    loom_integer_division_plan_t* plan,
    loom_integer_division_candidate_t* candidate) {
  candidate->plan = plan;
  candidate->plan_next = NULL;
  if (plan->last_candidate) {
    plan->last_candidate->plan_next = candidate;
  } else {
    plan->first_candidate = candidate;
  }
  plan->last_candidate = candidate;
  ++plan->candidate_count;
}

static void loom_integer_division_plan_append_chain(
    loom_integer_division_plan_t* plan,
    loom_integer_division_candidate_t* candidate) {
  for (; candidate; candidate = candidate->previous_same_role) {
    loom_integer_division_plan_append(plan, candidate);
  }
}

static loom_integer_division_plan_t* loom_integer_division_create_plan(
    loom_integer_division_region_planner_t* planner,
    loom_integer_division_family_t family,
    loom_integer_division_candidate_t* quotient,
    loom_integer_division_candidate_t* remainder, loom_op_t* anchor) {
  loom_integer_division_plan_t* plan = &planner->plans[planner->plan_count++];
  *plan = (loom_integer_division_plan_t){
      .anchor = anchor,
      .quotient_name_source =
          loom_integer_division_oldest_candidate(quotient)->result,
      .remainder_name_source =
          loom_integer_division_oldest_candidate(remainder)->result,
      .family = family,
  };
  loom_integer_division_plan_append_chain(plan, quotient);
  loom_integer_division_plan_append_chain(plan, remainder);
  loom_integer_division_planner_t* function_planner = planner->planner;
  if (function_planner->last_plan) {
    function_planner->last_plan->next = plan;
  } else {
    function_planner->first_plan = plan;
  }
  function_planner->last_plan = plan;
  return plan;
}

static iree_status_t loom_integer_division_observe_candidate(
    loom_integer_division_region_planner_t* planner, loom_op_t* op,
    loom_integer_division_family_t family, loom_integer_division_role_t role) {
  const loom_value_id_t* operands = loom_op_const_operands(op);
  if (loom_value_facts_is_exact(
          loom_value_fact_table_lookup(planner->planner->facts, operands[1]))) {
    return iree_ok_status();
  }

  loom_integer_division_candidate_t* candidate =
      &planner->candidates[planner->candidate_count++];
  *candidate = (loom_integer_division_candidate_t){
      .op = op,
      .result = loom_op_const_results(op)[0],
      .role = role,
  };
  loom_integer_division_key_t key = {
      .lhs = loom_value_fact_table_query_identity(planner->planner->facts,
                                                  operands[0]),
      .rhs = loom_value_fact_table_query_identity(planner->planner->facts,
                                                  operands[1]),
      .type =
          loom_module_value_type(planner->planner->module, candidate->result),
      .family = family,
  };
  key.hash = loom_integer_division_hash_key(&key);

  loom_integer_division_candidate_t* existing =
      loom_integer_division_find(planner, &key);
  if (!existing) {
    loom_integer_division_insert(planner, key, candidate);
    return iree_ok_status();
  }
  if (existing->plan) {
    if (loom_dominates_op(loom_dominance_walk_info(planner->dominance_walk),
                          existing->plan->anchor, op)) {
      loom_integer_division_plan_append(existing->plan, candidate);
    }
    loom_integer_division_insert(planner, key, candidate);
    return iree_ok_status();
  }
  if (existing->role == role) {
    candidate->previous_same_role = existing;
    loom_integer_division_insert(planner, key, candidate);
    return iree_ok_status();
  }

  loom_integer_division_candidate_t* quotient =
      role == LOOM_INTEGER_DIVISION_ROLE_QUOTIENT ? candidate : existing;
  loom_integer_division_candidate_t* remainder =
      role == LOOM_INTEGER_DIVISION_ROLE_REMAINDER ? candidate : existing;
  loom_op_t* anchor = NULL;
  if (loom_integer_division_family_is_signed(family)) {
    anchor = loom_integer_division_oldest_candidate(quotient)->op;
    if (!loom_dominates_op(
            loom_dominance_walk_info(planner->dominance_walk), anchor,
            loom_integer_division_oldest_candidate(remainder)->op) &&
        !loom_integer_division_chain_uses_dominated(planner, remainder,
                                                    anchor)) {
      // The quotient's stricter overflow domain cannot move above an earlier
      // signed remainder. Shadow those remainders with the quotient so a later
      // remainder can still combine at the quotient's original position.
      loom_integer_division_insert(planner, key, candidate);
      return iree_ok_status();
    }
  } else {
    anchor = loom_integer_division_oldest_candidate(existing)->op;
  }
  loom_integer_division_create_plan(planner, family, quotient, remainder,
                                    anchor);
  loom_integer_division_insert(planner, key, candidate);
  return iree_ok_status();
}

static iree_status_t loom_integer_division_plan_region(
    loom_integer_division_planner_t* planner, loom_region_t* region,
    iree_arena_allocator_t* region_arena) {
  loom_integer_division_inventory_t inventory;
  IREE_RETURN_IF_ERROR(loom_integer_division_inventory_region(
      planner->module, planner->facts, region, &inventory));
  if (inventory.candidate_count < 2) {
    return iree_ok_status();
  }

  loom_integer_division_region_planner_t region_planner = {
      .planner = planner,
  };
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      region_arena, inventory.scope_count, sizeof(*region_planner.scopes),
      (void**)&region_planner.scopes));
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      planner->plan_arena, inventory.candidate_count,
      sizeof(*region_planner.candidates), (void**)&region_planner.candidates));
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      planner->plan_arena, inventory.candidate_count / 2,
      sizeof(*region_planner.plans), (void**)&region_planner.plans));
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      region_arena, inventory.candidate_count, sizeof(*region_planner.bindings),
      (void**)&region_planner.bindings));
  const iree_host_size_t bucket_count = iree_host_size_next_power_of_two(
      (iree_host_size_t)inventory.candidate_count * 2);
  region_planner.bucket_mask = bucket_count - 1;
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      region_arena, bucket_count, sizeof(*region_planner.bucket_heads),
      (void**)&region_planner.bucket_heads));
  memset(region_planner.bucket_heads, 0xFF,
         bucket_count * sizeof(*region_planner.bucket_heads));

  IREE_RETURN_IF_ERROR(loom_dominance_walk_create(
      planner->module, region,
      (loom_dominance_walk_callbacks_t){
          .user_data = &region_planner,
          .enter_scope = loom_integer_division_enter_scope,
          .leave_scope = loom_integer_division_leave_scope,
      },
      region_arena, &region_planner.dominance_walk));
  while (true) {
    loom_dominance_walk_cursor_t cursor;
    IREE_RETURN_IF_ERROR(
        loom_dominance_walk_next(region_planner.dominance_walk, &cursor));
    if (!cursor.op) {
      break;
    }
    loom_integer_division_family_t family;
    loom_integer_division_role_t role;
    if (loom_integer_division_match(cursor.op, &family, &role)) {
      IREE_RETURN_IF_ERROR(loom_integer_division_observe_candidate(
          &region_planner, cursor.op, family, role));
    }
  }
  return iree_ok_status();
}

static iree_status_t loom_integer_division_build_combined(
    loom_rewriter_t* rewriter, loom_integer_division_plan_t* plan,
    loom_op_t** out_op) {
  const loom_value_id_t* operands = loom_op_const_operands(plan->anchor);
  const loom_type_t result_type = loom_module_value_type(
      rewriter->module, loom_op_const_results(plan->anchor)[0]);
  const loom_type_t result_types[2] = {result_type, result_type};
  loom_builder_set_before(&rewriter->builder, plan->anchor);
  switch (plan->family) {
    case LOOM_INTEGER_DIVISION_FAMILY_SCALAR_SIGNED:
      return loom_scalar_divremsi_build(&rewriter->builder, operands[0],
                                        operands[1], result_types,
                                        plan->anchor->location, out_op);
    case LOOM_INTEGER_DIVISION_FAMILY_SCALAR_UNSIGNED:
      return loom_scalar_divremui_build(&rewriter->builder, operands[0],
                                        operands[1], result_types,
                                        plan->anchor->location, out_op);
    case LOOM_INTEGER_DIVISION_FAMILY_INDEX:
      return loom_index_divrem_build(&rewriter->builder, operands[0],
                                     operands[1], plan->anchor->location,
                                     out_op);
    case LOOM_INTEGER_DIVISION_FAMILY_VECTOR_SIGNED:
      return loom_vector_divremsi_build(&rewriter->builder, operands[0],
                                        operands[1], result_types,
                                        plan->anchor->location, out_op);
    case LOOM_INTEGER_DIVISION_FAMILY_VECTOR_UNSIGNED:
      return loom_vector_divremui_build(&rewriter->builder, operands[0],
                                        operands[1], result_types,
                                        plan->anchor->location, out_op);
  }
  IREE_ASSERT_UNREACHABLE("invalid integer division family");
  IREE_BUILTIN_UNREACHABLE();
}

static iree_status_t loom_integer_division_apply_plan(
    loom_rewriter_t* rewriter, loom_integer_division_plan_t* plan) {
  loom_op_t* combined = NULL;
  IREE_RETURN_IF_ERROR(
      loom_integer_division_build_combined(rewriter, plan, &combined));
  const loom_value_id_t* results = loom_op_const_results(combined);
  IREE_RETURN_IF_ERROR(loom_rewriter_copy_value_name(
      rewriter, plan->quotient_name_source, results[0]));
  IREE_RETURN_IF_ERROR(loom_rewriter_copy_value_name(
      rewriter, plan->remainder_name_source, results[1]));
  for (loom_integer_division_candidate_t* candidate = plan->first_candidate;
       candidate; candidate = candidate->plan_next) {
    const loom_value_id_t replacement =
        results[candidate->role == LOOM_INTEGER_DIVISION_ROLE_REMAINDER];
    IREE_RETURN_IF_ERROR(loom_rewriter_replace_all_uses_with(
        rewriter, candidate->result, replacement));
  }
  for (loom_integer_division_candidate_t* candidate = plan->first_candidate;
       candidate; candidate = candidate->plan_next) {
    IREE_RETURN_IF_ERROR(loom_rewriter_erase(rewriter, candidate->op));
  }
  return iree_ok_status();
}

iree_status_t loom_integer_division_combine(
    loom_module_t* module, loom_func_like_t function,
    const loom_value_fact_table_t* facts, iree_arena_allocator_t* parent_arena,
    loom_integer_division_combine_result_t* out_result) {
  *out_result = (loom_integer_division_combine_result_t){0};
  if (!loom_func_like_body(function)) {
    return iree_ok_status();
  }
  if (!loom_integer_division_has_pair_candidates(module, facts)) {
    return iree_ok_status();
  }

  iree_arena_allocator_t plan_arena;
  iree_arena_allocator_t region_arena;
  iree_arena_initialize(parent_arena->block_pool, &plan_arena);
  iree_arena_initialize(parent_arena->block_pool, &region_arena);
  loom_integer_division_planner_t planner = {
      .module = module,
      .facts = facts,
      .plan_arena = &plan_arena,
  };
  iree_status_t status = iree_ok_status();
  for (uint8_t i = 0;
       i < loom_func_like_region_count(function) && iree_status_is_ok(status);
       ++i) {
    loom_region_t* region = loom_func_like_region(function, i);
    if (!region) {
      continue;
    }
    iree_arena_reset(&region_arena);
    status = loom_integer_division_plan_region(&planner, region, &region_arena);
  }

  loom_rewriter_t rewriter;
  bool rewriter_initialized = false;
  if (iree_status_is_ok(status) && planner.first_plan) {
    loom_rewriter_initialize(&rewriter, module, &plan_arena);
    rewriter_initialized = true;
    status = loom_rewriter_enable_worklist(&rewriter);
    for (loom_integer_division_plan_t* plan = planner.first_plan;
         plan && iree_status_is_ok(status); plan = plan->next) {
      status = loom_integer_division_apply_plan(&rewriter, plan);
      if (iree_status_is_ok(status)) {
        ++out_result->combined_operation_count;
        out_result->retired_operation_count += plan->candidate_count;
      }
    }
    while (iree_status_is_ok(status)) {
      loom_op_t* op = loom_rewriter_pop(&rewriter);
      if (!op) {
        break;
      }
      bool erased = false;
      status = loom_rewriter_erase_if_dead(&rewriter, op, &erased);
    }
  }
  if (rewriter_initialized) {
    loom_rewriter_deinitialize(&rewriter);
  }
  iree_arena_deinitialize(&region_arena);
  iree_arena_deinitialize(&plan_arena);
  return status;
}
