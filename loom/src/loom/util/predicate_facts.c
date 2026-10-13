// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/util/predicate_facts.h"

#include <stdint.h>

#include "iree/base/internal/math.h"
#include "loom/util/fact_ordinal_map.h"

// At most this many aliases bound each predicate's scan to constant work.
#define LOOM_VALUE_FACT_ALIAS_LINEAR_SCAN_LIMIT 16

// At most this many predicates bound the number of full alias-list scans.
#define LOOM_VALUE_FACT_PREDICATE_LINEAR_SCAN_LIMIT 4

uint16_t loom_value_facts_export_integer_refinements(
    loom_value_id_t value, loom_value_facts_t inferred_facts,
    loom_value_facts_t proven_facts, loom_predicate_t* predicates,
    uint16_t capacity) {
  if (capacity == 0 || value == LOOM_VALUE_ID_INVALID ||
      loom_value_facts_is_float(proven_facts) ||
      loom_value_facts_is_unknown(proven_facts)) {
    return 0;
  }

  uint16_t count = 0;
  if (loom_value_facts_is_float(inferred_facts) ||
      proven_facts.range_lo > inferred_facts.range_lo ||
      proven_facts.range_hi < inferred_facts.range_hi) {
    if (loom_value_facts_is_exact(proven_facts)) {
      predicates[count++] = (loom_predicate_t){
          .kind = LOOM_PREDICATE_EQ,
          .arg_count = 2,
          .arg_tags = {LOOM_PRED_ARG_VALUE, LOOM_PRED_ARG_CONST,
                       LOOM_PRED_ARG_NONE},
          .args = {value, proven_facts.range_lo, 0},
      };
    } else {
      predicates[count++] = (loom_predicate_t){
          .kind = LOOM_PREDICATE_RANGE,
          .arg_count = 3,
          .arg_tags = {LOOM_PRED_ARG_VALUE, LOOM_PRED_ARG_CONST,
                       LOOM_PRED_ARG_CONST},
          .args = {value, proven_facts.range_lo, proven_facts.range_hi},
      };
    }
  }

  if (count < capacity && proven_facts.known_divisor > 1 &&
      !loom_value_facts_is_exact(proven_facts) &&
      (loom_value_facts_is_float(inferred_facts) ||
       inferred_facts.known_divisor % proven_facts.known_divisor != 0)) {
    predicates[count++] = (loom_predicate_t){
        .kind = LOOM_PREDICATE_MULTIPLE_OF,
        .arg_count = 2,
        .arg_tags = {LOOM_PRED_ARG_VALUE, LOOM_PRED_ARG_CONST,
                     LOOM_PRED_ARG_NONE},
        .args = {value, proven_facts.known_divisor, 0},
    };
  }
  return count;
}

static bool loom_value_fact_alias_map_resolve(
    const loom_value_fact_table_t* table,
    const loom_value_fact_ordinal_map_t* alias_map,
    const loom_value_facts_t* alias_facts, loom_value_id_t value_id,
    bool* out_is_alias, uint16_t* out_alias_ordinal,
    loom_value_facts_t* out_facts) {
  if (out_is_alias) {
    *out_is_alias = false;
  }
  if (out_alias_ordinal) {
    *out_alias_ordinal = 0;
  }
  uint16_t alias_ordinal = 0;
  if (loom_value_fact_ordinal_map_find(alias_map, value_id, &alias_ordinal)) {
    if (out_is_alias) {
      *out_is_alias = true;
    }
    if (out_alias_ordinal) {
      *out_alias_ordinal = alias_ordinal;
    }
    *out_facts = alias_facts[alias_ordinal];
    return true;
  }
  return loom_value_fact_table_try_lookup(table, value_id, out_facts);
}

static void loom_value_fact_alias_map_refine_relation(
    const loom_value_fact_table_t* table,
    const loom_value_fact_ordinal_map_t* alias_map, uint8_t predicate_kind,
    loom_value_id_t lhs, loom_value_id_t rhs, loom_value_facts_t* inout_facts) {
  bool lhs_is_alias = false;
  bool rhs_is_alias = false;
  uint16_t lhs_ordinal = 0;
  uint16_t rhs_ordinal = 0;
  loom_value_facts_t lhs_facts = loom_value_facts_unknown();
  loom_value_facts_t rhs_facts = loom_value_facts_unknown();
  if (!loom_value_fact_alias_map_resolve(table, alias_map, inout_facts, lhs,
                                         &lhs_is_alias, &lhs_ordinal,
                                         &lhs_facts) ||
      !loom_value_fact_alias_map_resolve(table, alias_map, inout_facts, rhs,
                                         &rhs_is_alias, &rhs_ordinal,
                                         &rhs_facts) ||
      (!lhs_is_alias && !rhs_is_alias) ||
      loom_value_facts_is_float(lhs_facts) ||
      loom_value_facts_is_float(rhs_facts)) {
    return;
  }

  loom_value_facts_t* lhs_result =
      lhs_is_alias ? &inout_facts[lhs_ordinal] : NULL;
  loom_value_facts_t* rhs_result =
      rhs_is_alias ? &inout_facts[rhs_ordinal] : NULL;
  (void)loom_value_facts_refine_relation(predicate_kind, lhs_facts, rhs_facts,
                                         lhs_result, rhs_result);
}

static void loom_value_fact_table_apply_alias_predicates_with_map(
    const loom_value_fact_table_t* table,
    const loom_value_fact_ordinal_map_t* alias_map,
    const loom_predicate_t* predicates, uint16_t predicate_count,
    loom_value_facts_t* inout_facts) {
  // Apply predicates whose bounds are already literals or exact aliases.
  // Predicates retaining non-exact value operands are consumed by the
  // relational interval refinement below.
  for (uint16_t i = 0; i < predicate_count; ++i) {
    const loom_predicate_t* predicate = &predicates[i];
    if (predicate->arg_count == 0 ||
        predicate->arg_tags[0] != LOOM_PRED_ARG_VALUE ||
        predicate->args[0] < 0) {
      continue;
    }
    uint16_t target_ordinal = 0;
    if (!loom_value_fact_ordinal_map_find(
            alias_map, (loom_value_id_t)predicate->args[0], &target_ordinal)) {
      continue;
    }

    loom_predicate_t resolved_predicate = *predicate;
    for (uint8_t argument_index = 1;
         argument_index < resolved_predicate.arg_count; ++argument_index) {
      if (resolved_predicate.arg_tags[argument_index] != LOOM_PRED_ARG_VALUE ||
          resolved_predicate.args[argument_index] < 0) {
        continue;
      }
      loom_value_facts_t argument_facts = loom_value_facts_unknown();
      if (!loom_value_fact_alias_map_resolve(
              table, alias_map, inout_facts,
              (loom_value_id_t)resolved_predicate.args[argument_index],
              /*out_is_alias=*/NULL, /*out_alias_ordinal=*/NULL,
              &argument_facts)) {
        continue;
      }
      int64_t exact_value = 0;
      if (!loom_value_facts_as_exact_i64(argument_facts, &exact_value)) {
        continue;
      }
      resolved_predicate.arg_tags[argument_index] = LOOM_PRED_ARG_CONST;
      resolved_predicate.args[argument_index] = exact_value;
    }
    loom_value_facts_apply_predicate(&inout_facts[target_ordinal],
                                     &resolved_predicate);
  }

  // Relational predicates constrain aliases from the known interval of their
  // counterpart. RANGE decomposes into lower <= value <= upper so each dynamic
  // endpoint contributes independently. External facts are read-only; only
  // aliases have result slots.
  for (uint16_t i = 0; i < predicate_count; ++i) {
    const loom_predicate_t* predicate = &predicates[i];
    if (predicate->kind == LOOM_PREDICATE_RANGE) {
      if (predicate->arg_count == 3 &&
          predicate->arg_tags[0] == LOOM_PRED_ARG_VALUE &&
          predicate->args[0] >= 0) {
        // Literal endpoints were already applied above. Only dynamic
        // endpoints require relational refinement.
        if (predicate->arg_tags[1] == LOOM_PRED_ARG_VALUE &&
            predicate->args[1] >= 0) {
          loom_value_fact_alias_map_refine_relation(
              table, alias_map, LOOM_PREDICATE_LE,
              (loom_value_id_t)predicate->args[1],
              (loom_value_id_t)predicate->args[0], inout_facts);
        }
        if (predicate->arg_tags[2] == LOOM_PRED_ARG_VALUE &&
            predicate->args[2] >= 0) {
          loom_value_fact_alias_map_refine_relation(
              table, alias_map, LOOM_PREDICATE_LE,
              (loom_value_id_t)predicate->args[0],
              (loom_value_id_t)predicate->args[2], inout_facts);
        }
      }
      continue;
    }
    if (predicate->arg_count != 2 ||
        predicate->arg_tags[0] != LOOM_PRED_ARG_VALUE ||
        predicate->arg_tags[1] != LOOM_PRED_ARG_VALUE ||
        predicate->args[0] < 0 || predicate->args[1] < 0) {
      continue;
    }
    loom_value_fact_alias_map_refine_relation(
        table, alias_map, predicate->kind, (loom_value_id_t)predicate->args[0],
        (loom_value_id_t)predicate->args[1], inout_facts);
  }
}

iree_status_t loom_value_fact_table_apply_alias_predicates(
    loom_value_fact_table_t* table, const loom_value_id_t* values,
    uint16_t value_count, const loom_predicate_t* predicates,
    uint16_t predicate_count, loom_value_facts_t* inout_facts) {
  const bool use_direct_map =
      value_count > LOOM_VALUE_FACT_ALIAS_LINEAR_SCAN_LIMIT &&
      predicate_count > LOOM_VALUE_FACT_PREDICATE_LINEAR_SCAN_LIMIT;
  loom_value_fact_ordinal_map_t alias_map = {0};
  IREE_RETURN_IF_ERROR(loom_value_fact_ordinal_map_initialize(
      table, values, value_count, use_direct_map, &alias_map));

  loom_value_fact_table_apply_alias_predicates_with_map(
      table, &alias_map, predicates, predicate_count, inout_facts);
  loom_value_fact_ordinal_map_reset(&alias_map);
  return iree_ok_status();
}
