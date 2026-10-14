// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/util/fact_relation.h"

#include <string.h>

#include "iree/base/internal/math.h"
#include "loom/util/fact_ordinal_map.h"

#define LOOM_VALUE_FACT_RELATION_INITIAL_CAPACITY 8
#define LOOM_VALUE_FACT_RELATION_STACK_CAPACITY 16
#define LOOM_VALUE_FACT_RELATION_DIRECT_PREDICATE_COUNT 4

typedef struct loom_value_fact_relation_roots_t {
  loom_value_set_id_t excluded[LOOM_INTEGER_ORDER_OUTCOME_COUNT];
} loom_value_fact_relation_roots_t;

typedef struct loom_value_fact_relation_entry_t {
  loom_value_id_t value_id;
  loom_value_fact_relation_roots_t roots;
} loom_value_fact_relation_entry_t;

static_assert(sizeof(loom_value_fact_relation_roots_t) == 12,
              "three canonical set roots must remain 12 bytes");
static_assert(sizeof(loom_value_fact_relation_entry_t) == 16,
              "sparse relation entries must remain 16 bytes");

struct loom_value_fact_integer_relations_t {
  // Canonical counterpart-identity sets shared by every sparse entry.
  loom_value_set_index_t* index;

  // Sparse entries sorted by scoped alias value ID.
  loom_value_fact_relation_entry_t* entries;

  // Number of sparse entries, including reusable zero-root tombstones.
  iree_host_size_t count;

  // Allocated sparse entry capacity.
  iree_host_size_t capacity;

  // Reusable storage for identity-operation plans larger than the stack path.
  struct {
    // Canonical source identity per result ordinal.
    loom_value_id_t* identities;

    // Inherited roots and completed roots per result ordinal.
    loom_value_fact_relation_roots_t* plans;

    // Predicate-local roots keyed by first canonical identity ordinal.
    loom_value_fact_relation_roots_t* locals;

    // Allocated entries in each parallel scratch array.
    uint16_t capacity;
  } scratch;
};

static bool loom_value_fact_relation_roots_equal(
    loom_value_fact_relation_roots_t lhs,
    loom_value_fact_relation_roots_t rhs) {
  for (uint8_t i = 0; i < LOOM_INTEGER_ORDER_OUTCOME_COUNT; ++i) {
    if (lhs.excluded[i] != rhs.excluded[i]) {
      return false;
    }
  }
  return true;
}

static bool loom_value_fact_relation_roots_empty(
    loom_value_fact_relation_roots_t roots) {
  return roots.excluded[0] == 0 && roots.excluded[1] == 0 &&
         roots.excluded[2] == 0;
}

static iree_host_size_t loom_value_fact_relation_entry_lower_bound(
    const loom_value_fact_integer_relations_t* relations,
    loom_value_id_t value_id, bool* out_found) {
  iree_host_size_t lower = 0;
  iree_host_size_t upper = relations ? relations->count : 0;
  while (lower < upper) {
    const iree_host_size_t middle = lower + (upper - lower) / 2;
    if (relations->entries[middle].value_id < value_id) {
      lower = middle + 1;
    } else {
      upper = middle;
    }
  }
  *out_found = relations && lower < relations->count &&
               relations->entries[lower].value_id == value_id;
  return lower;
}

static loom_value_fact_relation_roots_t loom_value_fact_relation_lookup_roots(
    const loom_value_fact_table_t* table, loom_value_id_t value_id) {
  if (!table || !table->integer_relations) {
    return (loom_value_fact_relation_roots_t){0};
  }
  bool found = false;
  const iree_host_size_t ordinal = loom_value_fact_relation_entry_lower_bound(
      table->integer_relations, value_id, &found);
  return found ? table->integer_relations->entries[ordinal].roots
               : (loom_value_fact_relation_roots_t){0};
}

static iree_status_t loom_value_fact_integer_relations_ensure_owner(
    loom_value_fact_table_t* table,
    loom_value_fact_integer_relations_t** out_relations) {
  if (!table->integer_relations) {
    loom_value_fact_integer_relations_t* relations = NULL;
    IREE_RETURN_IF_ERROR(iree_arena_allocate(
        table->transient_arena, sizeof(*relations), (void**)&relations));
    memset(relations, 0, sizeof(*relations));
    table->integer_relations = relations;
  }
  *out_relations = table->integer_relations;
  return iree_ok_status();
}

static iree_status_t loom_value_fact_integer_relations_ensure_index(
    loom_value_fact_table_t* table,
    loom_value_fact_integer_relations_t** out_relations) {
  IREE_RETURN_IF_ERROR(
      loom_value_fact_integer_relations_ensure_owner(table, out_relations));
  if (!(*out_relations)->index) {
    IREE_RETURN_IF_ERROR(loom_value_set_index_allocate(
        table->transient_arena, &(*out_relations)->index));
  }
  return iree_ok_status();
}

static iree_status_t loom_value_fact_integer_relations_reserve_entries(
    loom_value_fact_table_t* table, iree_host_size_t required_capacity) {
  loom_value_fact_integer_relations_t* relations = NULL;
  IREE_RETURN_IF_ERROR(
      loom_value_fact_integer_relations_ensure_owner(table, &relations));
  if (required_capacity <= relations->capacity) {
    return iree_ok_status();
  }
  iree_host_size_t new_capacity = LOOM_VALUE_FACT_RELATION_INITIAL_CAPACITY;
  if (relations->capacity &&
      !iree_host_size_checked_mul(relations->capacity, 2, &new_capacity)) {
    return iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                            "relation entry capacity overflow");
  }
  new_capacity = iree_max(new_capacity, required_capacity);
  return iree_arena_grow_array(table->transient_arena, relations->count,
                               new_capacity, sizeof(*relations->entries),
                               &relations->capacity,
                               (void**)&relations->entries);
}

// Publishes into pre-reserved storage and returns whether roots changed.
static bool loom_value_fact_integer_relations_publish_reserved(
    loom_value_fact_integer_relations_t* relations, loom_value_id_t value_id,
    loom_value_fact_relation_roots_t roots) {
  bool found = false;
  const iree_host_size_t ordinal =
      loom_value_fact_relation_entry_lower_bound(relations, value_id, &found);
  if (found) {
    loom_value_fact_relation_entry_t* entry = &relations->entries[ordinal];
    if (loom_value_fact_relation_roots_equal(entry->roots, roots)) {
      return false;
    }
    entry->roots = roots;
    return true;
  }
  if (loom_value_fact_relation_roots_empty(roots)) {
    return false;
  }
  IREE_ASSERT_LT(relations->count, relations->capacity);
  memmove(&relations->entries[ordinal + 1], &relations->entries[ordinal],
          (relations->count - ordinal) * sizeof(*relations->entries));
  relations->entries[ordinal] = (loom_value_fact_relation_entry_t){
      .value_id = value_id,
      .roots = roots,
  };
  ++relations->count;
  return true;
}

static iree_status_t loom_value_fact_integer_relations_publish_one(
    loom_value_fact_table_t* table, loom_value_id_t value_id,
    loom_value_fact_relation_roots_t roots, bool* out_changed) {
  *out_changed = false;
  bool found = false;
  if (table->integer_relations) {
    (void)loom_value_fact_relation_entry_lower_bound(table->integer_relations,
                                                     value_id, &found);
  }
  if (!found && loom_value_fact_relation_roots_empty(roots)) {
    return iree_ok_status();
  }
  const iree_host_size_t required_capacity =
      table->integer_relations ? table->integer_relations->count + !found : 1;
  IREE_RETURN_IF_ERROR(loom_value_fact_integer_relations_reserve_entries(
      table, required_capacity));
  *out_changed = loom_value_fact_integer_relations_publish_reserved(
      table->integer_relations, value_id, roots);
  return iree_ok_status();
}

static iree_status_t loom_value_fact_relation_add_exclusions(
    loom_value_fact_table_t* table,
    loom_value_fact_relation_roots_t* inout_roots,
    loom_integer_order_outcome_bits_t allowed_outcomes,
    loom_value_id_t counterpart_identity) {
  const loom_integer_order_outcome_bits_t excluded_outcomes =
      LOOM_INTEGER_ORDER_OUTCOME_BIT_ALL & ~allowed_outcomes;
  if (!excluded_outcomes) {
    return iree_ok_status();
  }
  loom_value_fact_integer_relations_t* relations = NULL;
  IREE_RETURN_IF_ERROR(
      loom_value_fact_integer_relations_ensure_index(table, &relations));
  for (uint8_t outcome = 0; outcome < LOOM_INTEGER_ORDER_OUTCOME_COUNT;
       ++outcome) {
    if (!(excluded_outcomes & (1u << outcome))) {
      continue;
    }
    IREE_RETURN_IF_ERROR(loom_value_set_index_add(
        relations->index, inout_roots->excluded[outcome], counterpart_identity,
        &inout_roots->excluded[outcome]));
  }
  return iree_ok_status();
}

static iree_status_t loom_value_fact_relation_union_roots(
    loom_value_fact_table_t* table, loom_value_fact_relation_roots_t source,
    loom_value_fact_relation_roots_t* inout_target) {
  if (loom_value_fact_relation_roots_empty(source)) {
    return iree_ok_status();
  }
  loom_value_fact_integer_relations_t* relations = NULL;
  IREE_RETURN_IF_ERROR(
      loom_value_fact_integer_relations_ensure_owner(table, &relations));
  IREE_ASSERT(relations->index);
  for (uint8_t i = 0; i < LOOM_INTEGER_ORDER_OUTCOME_COUNT; ++i) {
    IREE_RETURN_IF_ERROR(loom_value_set_index_union(
        relations->index, inout_target->excluded[i], source.excluded[i],
        &inout_target->excluded[i]));
  }
  return iree_ok_status();
}

loom_integer_order_outcome_bits_t
loom_value_fact_table_query_signed_integer_order(
    const loom_value_fact_table_t* table, loom_value_id_t lhs,
    loom_value_id_t rhs) {
  if (lhs == LOOM_VALUE_ID_INVALID || rhs == LOOM_VALUE_ID_INVALID) {
    return LOOM_INTEGER_ORDER_OUTCOME_BIT_ALL;
  }
  if (lhs == rhs) {
    return LOOM_INTEGER_ORDER_OUTCOME_BIT_EQUAL;
  }
  if (!table) {
    return LOOM_INTEGER_ORDER_OUTCOME_BIT_ALL;
  }

  const loom_value_id_t lhs_identity =
      loom_value_fact_table_query_identity(table, lhs);
  const loom_value_id_t rhs_identity =
      loom_value_fact_table_query_identity(table, rhs);
  if (lhs_identity == rhs_identity) {
    return LOOM_INTEGER_ORDER_OUTCOME_BIT_EQUAL;
  }

  const loom_value_facts_t lhs_facts = loom_value_fact_table_lookup(table, lhs);
  const loom_value_facts_t rhs_facts = loom_value_fact_table_lookup(table, rhs);
  if (loom_value_facts_is_exact(lhs_facts) &&
      loom_value_facts_is_exact(rhs_facts) &&
      !loom_value_facts_is_float(lhs_facts) &&
      !loom_value_facts_is_float(rhs_facts)) {
    if (lhs_facts.range_lo < rhs_facts.range_lo) {
      return LOOM_INTEGER_ORDER_OUTCOME_BIT_LESS;
    }
    if (lhs_facts.range_lo > rhs_facts.range_lo) {
      return LOOM_INTEGER_ORDER_OUTCOME_BIT_GREATER;
    }
    return LOOM_INTEGER_ORDER_OUTCOME_BIT_EQUAL;
  }

  if (!table->integer_relations) {
    return LOOM_INTEGER_ORDER_OUTCOME_BIT_ALL;
  }
  loom_integer_order_outcome_bits_t outcomes =
      LOOM_INTEGER_ORDER_OUTCOME_BIT_ALL;
  const loom_value_fact_relation_roots_t lhs_roots =
      loom_value_fact_relation_lookup_roots(table, lhs);
  const loom_value_fact_relation_roots_t rhs_roots =
      loom_value_fact_relation_lookup_roots(table, rhs);
  for (uint8_t outcome = 0; outcome < LOOM_INTEGER_ORDER_OUTCOME_COUNT;
       ++outcome) {
    if (lhs_roots.excluded[outcome] &&
        loom_value_set_index_contains(table->integer_relations->index,
                                      lhs_roots.excluded[outcome],
                                      rhs_identity)) {
      outcomes &= ~(1u << outcome);
    }
    const loom_integer_order_outcome_t swapped =
        loom_integer_order_outcome_swap(outcome);
    if (rhs_roots.excluded[swapped] &&
        loom_value_set_index_contains(table->integer_relations->index,
                                      rhs_roots.excluded[swapped],
                                      lhs_identity)) {
      outcomes &= ~(1u << outcome);
    }
  }
  return outcomes ? outcomes : LOOM_INTEGER_ORDER_OUTCOME_BIT_ALL;
}

void loom_value_fact_integer_relations_clear_scope(
    loom_value_fact_table_t* table) {
  table->integer_relations = NULL;
}

void loom_value_fact_integer_relations_undefine(loom_value_fact_table_t* table,
                                                loom_value_id_t value_id) {
  if (!table->integer_relations) {
    return;
  }
  bool found = false;
  const iree_host_size_t ordinal = loom_value_fact_relation_entry_lower_bound(
      table->integer_relations, value_id, &found);
  if (found) {
    table->integer_relations->entries[ordinal].roots =
        (loom_value_fact_relation_roots_t){0};
  }
}

iree_status_t loom_value_fact_integer_relations_clone_value(
    loom_value_fact_table_t* target, const loom_value_fact_table_t* source,
    loom_value_id_t value_id) {
  if (target == source) {
    return iree_ok_status();
  }
  const loom_value_fact_relation_roots_t source_roots =
      loom_value_fact_relation_lookup_roots(source, value_id);
  if (loom_value_fact_relation_roots_empty(source_roots)) {
    bool changed = false;
    return loom_value_fact_integer_relations_publish_one(
        target, value_id, source_roots, &changed);
  }

  loom_value_fact_relation_roots_t target_roots = {0};
  loom_value_fact_integer_relations_t* target_relations = NULL;
  IREE_RETURN_IF_ERROR(loom_value_fact_integer_relations_ensure_index(
      target, &target_relations));
  for (uint8_t outcome = 0; outcome < LOOM_INTEGER_ORDER_OUTCOME_COUNT;
       ++outcome) {
    loom_value_set_cursor_t cursor;
    loom_value_set_cursor_begin(source->integer_relations->index,
                                source_roots.excluded[outcome], &cursor);
    for (loom_value_id_t counterpart = loom_value_set_cursor_next(&cursor);
         counterpart != LOOM_VALUE_ID_INVALID;
         counterpart = loom_value_set_cursor_next(&cursor)) {
      IREE_RETURN_IF_ERROR(loom_value_set_index_add(
          target_relations->index, target_roots.excluded[outcome], counterpart,
          &target_roots.excluded[outcome]));
    }
  }
  bool changed = false;
  return loom_value_fact_integer_relations_publish_one(target, value_id,
                                                       target_roots, &changed);
}

static iree_status_t loom_value_fact_integer_relations_ensure_scratch(
    loom_value_fact_table_t* table, uint16_t count,
    loom_value_id_t** out_identities,
    loom_value_fact_relation_roots_t** out_plans,
    loom_value_fact_relation_roots_t** out_locals) {
  loom_value_fact_integer_relations_t* relations = NULL;
  IREE_RETURN_IF_ERROR(
      loom_value_fact_integer_relations_ensure_owner(table, &relations));
  if (count > relations->scratch.capacity) {
    loom_value_id_t* identities = NULL;
    loom_value_fact_relation_roots_t* plans = NULL;
    loom_value_fact_relation_roots_t* locals = NULL;
    IREE_RETURN_IF_ERROR(iree_arena_allocate_array(table->transient_arena,
                                                   count, sizeof(*identities),
                                                   (void**)&identities));
    IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
        table->transient_arena, count, sizeof(*plans), (void**)&plans));
    IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
        table->transient_arena, count, sizeof(*locals), (void**)&locals));
    relations->scratch.identities = identities;
    relations->scratch.plans = plans;
    relations->scratch.locals = locals;
    relations->scratch.capacity = count;
  }
  *out_identities = relations->scratch.identities;
  *out_plans = relations->scratch.plans;
  *out_locals = relations->scratch.locals;
  return iree_ok_status();
}

typedef struct loom_value_fact_relation_plan_t {
  loom_value_id_t* identities;
  loom_value_fact_relation_roots_t* plans;
  loom_value_fact_relation_roots_t* locals;
  loom_value_fact_ordinal_map_t identity_map;
  bool initialized;
} loom_value_fact_relation_plan_t;

static iree_status_t loom_value_fact_relation_plan_initialize(
    loom_value_fact_table_t* table, const loom_value_id_t* operands,
    uint16_t pair_count, iree_host_size_t predicate_count, bool use_stack,
    loom_value_id_t* stack_identities,
    loom_value_fact_relation_roots_t* stack_plans,
    loom_value_fact_relation_roots_t* stack_locals,
    loom_value_fact_relation_plan_t* plan) {
  if (use_stack) {
    plan->identities = stack_identities;
    plan->plans = stack_plans;
    plan->locals = stack_locals;
  } else {
    IREE_RETURN_IF_ERROR(loom_value_fact_integer_relations_ensure_scratch(
        table, pair_count, &plan->identities, &plan->plans, &plan->locals));
  }
  for (uint16_t i = 0; i < pair_count; ++i) {
    plan->identities[i] =
        loom_value_fact_table_query_identity(table, operands[i]);
    plan->plans[i] = loom_value_fact_relation_lookup_roots(table, operands[i]);
    plan->locals[i] = (loom_value_fact_relation_roots_t){0};
  }
  const bool use_direct_map =
      pair_count > LOOM_VALUE_FACT_RELATION_STACK_CAPACITY &&
      predicate_count > LOOM_VALUE_FACT_RELATION_DIRECT_PREDICATE_COUNT;
  IREE_RETURN_IF_ERROR(loom_value_fact_ordinal_map_initialize(
      table, plan->identities, pair_count, use_direct_map,
      &plan->identity_map));
  plan->initialized = true;
  return iree_ok_status();
}

static iree_status_t loom_value_fact_relation_plan_add(
    loom_value_fact_table_t* table, loom_value_fact_relation_plan_t* plan,
    loom_value_id_t lhs, loom_symbolic_integer_relation_t relation,
    loom_value_id_t rhs) {
  const loom_value_id_t lhs_identity =
      loom_value_fact_table_query_identity(table, lhs);
  const loom_value_id_t rhs_identity =
      loom_value_fact_table_query_identity(table, rhs);
  uint16_t ordinal = 0;
  if (loom_value_fact_ordinal_map_find(&plan->identity_map, lhs_identity,
                                       &ordinal)) {
    IREE_RETURN_IF_ERROR(loom_value_fact_relation_add_exclusions(
        table, &plan->locals[ordinal],
        loom_symbolic_integer_relation_outcomes(relation), rhs_identity));
  }
  if (loom_value_fact_ordinal_map_find(&plan->identity_map, rhs_identity,
                                       &ordinal)) {
    IREE_RETURN_IF_ERROR(loom_value_fact_relation_add_exclusions(
        table, &plan->locals[ordinal],
        loom_symbolic_integer_relation_outcomes(
            loom_symbolic_integer_relation_swap(relation)),
        lhs_identity));
  }
  return iree_ok_status();
}

static bool loom_value_fact_relation_value_is_integer(
    const loom_module_t* module, int64_t value) {
  if (value < 0) {
    return false;
  }
  const loom_type_t type =
      loom_module_value_type(module, (loom_value_id_t)value);
  return !loom_scalar_type_is_float(loom_type_element_type(type));
}

static bool loom_value_fact_relation_predicate_is_supported(
    const loom_module_t* module, const loom_predicate_t* predicate) {
  if (predicate->kind == LOOM_PREDICATE_RANGE) {
    return predicate->arg_count == 3 &&
           predicate->arg_tags[0] == LOOM_PRED_ARG_VALUE &&
           loom_value_fact_relation_value_is_integer(module,
                                                     predicate->args[0]) &&
           ((predicate->arg_tags[1] == LOOM_PRED_ARG_VALUE &&
             loom_value_fact_relation_value_is_integer(module,
                                                       predicate->args[1])) ||
            (predicate->arg_tags[2] == LOOM_PRED_ARG_VALUE &&
             loom_value_fact_relation_value_is_integer(module,
                                                       predicate->args[2])));
  }
  loom_symbolic_integer_relation_t relation = LOOM_SYMBOLIC_INTEGER_RELATION_EQ;
  return predicate->arg_count == 2 &&
         predicate->arg_tags[0] == LOOM_PRED_ARG_VALUE &&
         predicate->arg_tags[1] == LOOM_PRED_ARG_VALUE &&
         loom_value_fact_relation_value_is_integer(module,
                                                   predicate->args[0]) &&
         loom_value_fact_relation_value_is_integer(module,
                                                   predicate->args[1]) &&
         loom_symbolic_integer_relation_from_predicate_kind(predicate->kind,
                                                            &relation);
}

static iree_status_t loom_value_fact_relation_plan_add_predicate(
    loom_value_fact_table_t* table, loom_value_fact_relation_plan_t* plan,
    const loom_predicate_t* predicate) {
  if (predicate->kind == LOOM_PREDICATE_RANGE) {
    const loom_value_id_t value = (loom_value_id_t)predicate->args[0];
    if (predicate->arg_tags[1] == LOOM_PRED_ARG_VALUE &&
        predicate->args[1] >= 0) {
      IREE_RETURN_IF_ERROR(loom_value_fact_relation_plan_add(
          table, plan, (loom_value_id_t)predicate->args[1],
          LOOM_SYMBOLIC_INTEGER_RELATION_LE, value));
    }
    if (predicate->arg_tags[2] == LOOM_PRED_ARG_VALUE &&
        predicate->args[2] >= 0) {
      IREE_RETURN_IF_ERROR(loom_value_fact_relation_plan_add(
          table, plan, value, LOOM_SYMBOLIC_INTEGER_RELATION_LE,
          (loom_value_id_t)predicate->args[2]));
    }
    return iree_ok_status();
  }
  loom_symbolic_integer_relation_t relation = LOOM_SYMBOLIC_INTEGER_RELATION_EQ;
  const bool converted = loom_symbolic_integer_relation_from_predicate_kind(
      predicate->kind, &relation);
  IREE_ASSERT(converted);
  return loom_value_fact_relation_plan_add(
      table, plan, (loom_value_id_t)predicate->args[0], relation,
      (loom_value_id_t)predicate->args[1]);
}

static bool loom_value_fact_integer_relations_identity_needs_plan(
    const loom_value_fact_table_t* table, const loom_value_id_t* operands,
    const loom_value_id_t* results, uint16_t pair_count) {
  if (!table->integer_relations) {
    return false;
  }
  for (uint16_t i = 0; i < pair_count; ++i) {
    if (!loom_value_fact_relation_roots_empty(
            loom_value_fact_relation_lookup_roots(table, operands[i])) ||
        !loom_value_fact_relation_roots_empty(
            loom_value_fact_relation_lookup_roots(table, results[i]))) {
      return true;
    }
  }
  return false;
}

static iree_status_t loom_value_fact_integer_relations_publish_plan(
    loom_value_fact_table_t* table, const loom_value_id_t* results,
    uint16_t pair_count, loom_value_fact_relation_plan_t* plan,
    bool* inout_changed) {
  if (!table->integer_relations) {
    return iree_ok_status();
  }
  iree_host_size_t missing_count = 0;
  for (uint16_t i = 0; i < pair_count; ++i) {
    uint16_t group_ordinal = 0;
    const bool found_group = loom_value_fact_ordinal_map_find(
        &plan->identity_map, plan->identities[i], &group_ordinal);
    IREE_ASSERT(found_group);
    IREE_RETURN_IF_ERROR(loom_value_fact_relation_union_roots(
        table, plan->locals[group_ordinal], &plan->plans[i]));

    bool found_entry = false;
    (void)loom_value_fact_relation_entry_lower_bound(table->integer_relations,
                                                     results[i], &found_entry);
    if (!found_entry && !loom_value_fact_relation_roots_empty(plan->plans[i])) {
      ++missing_count;
    }
  }
  IREE_RETURN_IF_ERROR(loom_value_fact_integer_relations_reserve_entries(
      table, table->integer_relations->count + missing_count));
  for (uint16_t i = 0; i < pair_count; ++i) {
    if (loom_value_fact_integer_relations_publish_reserved(
            table->integer_relations, results[i], plan->plans[i]) &&
        inout_changed) {
      *inout_changed = true;
    }
  }
  return iree_ok_status();
}

static iree_status_t loom_value_fact_integer_relations_propagate_identity(
    loom_value_fact_table_t* table, const loom_module_t* module,
    const loom_op_t* op, bool* inout_changed) {
  const uint16_t pair_count = iree_min(op->operand_count, op->result_count);
  if (pair_count == 0) {
    return iree_ok_status();
  }
  const loom_value_id_t* operands = loom_op_const_operands(op);
  const loom_value_id_t* results = loom_op_const_results(op);

  iree_host_size_t predicate_count = 0;
  const loom_attribute_t* attributes = loom_op_const_attrs(op);
  for (uint8_t i = 0; i < op->attribute_count; ++i) {
    if (attributes[i].kind == LOOM_ATTR_PREDICATE_LIST) {
      predicate_count += attributes[i].count;
    }
  }

  loom_value_id_t stack_identities[LOOM_VALUE_FACT_RELATION_STACK_CAPACITY];
  loom_value_fact_relation_roots_t
      stack_plans[LOOM_VALUE_FACT_RELATION_STACK_CAPACITY];
  loom_value_fact_relation_roots_t
      stack_locals[LOOM_VALUE_FACT_RELATION_STACK_CAPACITY];
  loom_value_fact_relation_plan_t plan = {0};
  const bool use_stack = pair_count <= LOOM_VALUE_FACT_RELATION_STACK_CAPACITY;
  iree_status_t status = iree_ok_status();
  if (loom_value_fact_integer_relations_identity_needs_plan(
          table, operands, results, pair_count)) {
    status = loom_value_fact_relation_plan_initialize(
        table, operands, pair_count, predicate_count, use_stack,
        stack_identities, stack_plans, stack_locals, &plan);
  }

  for (uint8_t i = 0; i < op->attribute_count && iree_status_is_ok(status);
       ++i) {
    if (attributes[i].kind != LOOM_ATTR_PREDICATE_LIST) {
      continue;
    }
    for (uint16_t j = 0; j < attributes[i].count && iree_status_is_ok(status);
         ++j) {
      const loom_predicate_t* predicate = &attributes[i].predicate_list[j];
      if (!loom_value_fact_relation_predicate_is_supported(module, predicate)) {
        continue;
      }
      if (!plan.initialized) {
        status = loom_value_fact_relation_plan_initialize(
            table, operands, pair_count, predicate_count, use_stack,
            stack_identities, stack_plans, stack_locals, &plan);
      }
      if (iree_status_is_ok(status)) {
        status = loom_value_fact_relation_plan_add_predicate(table, &plan,
                                                             predicate);
      }
    }
  }
  if (iree_status_is_ok(status) && plan.initialized) {
    status = loom_value_fact_integer_relations_publish_plan(
        table, results, pair_count, &plan, inout_changed);
  }
  loom_value_fact_ordinal_map_reset(&plan.identity_map);
  return status;
}

static iree_status_t loom_value_fact_integer_relations_forward_alias(
    loom_value_fact_table_t* table, loom_value_id_t source,
    loom_value_id_t result, bool* inout_changed) {
  const loom_value_fact_relation_roots_t source_roots =
      loom_value_fact_relation_lookup_roots(table, source);
  const loom_value_fact_relation_roots_t result_roots =
      loom_value_fact_relation_lookup_roots(table, result);
  if (loom_value_fact_relation_roots_equal(source_roots, result_roots)) {
    return iree_ok_status();
  }
  bool changed = false;
  IREE_RETURN_IF_ERROR(loom_value_fact_integer_relations_publish_one(
      table, result, source_roots, &changed));
  if (changed && inout_changed) {
    *inout_changed = true;
  }
  return iree_ok_status();
}

iree_status_t loom_value_fact_integer_relations_propagate_origins(
    loom_value_fact_table_t* table, const loom_module_t* module,
    const loom_op_t* op, loom_trait_flags_t traits, bool* inout_changed) {
  if (loom_traits_are_fact_identity(traits)) {
    return loom_value_fact_integer_relations_propagate_identity(
        table, module, op, inout_changed);
  }
  if (loom_traits_are_value_alias(traits) && op->operand_count >= 1 &&
      op->result_count >= 1) {
    return loom_value_fact_integer_relations_forward_alias(
        table, loom_op_const_operands(op)[0], loom_op_const_results(op)[0],
        inout_changed);
  }
  return iree_ok_status();
}
