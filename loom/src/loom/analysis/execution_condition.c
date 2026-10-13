// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/analysis/execution_condition.h"

#include "loom/ir/context.h"
#include "loom/ir/module.h"
#include "loom/ops/op_defs.h"
#include "loom/target/facts.h"
#include "loom/util/fact_extensions.h"
#include "loom/util/fact_table.h"

typedef struct loom_execution_integer_facts_t {
  // Inclusive scalar or aggregate-element range.
  loom_value_facts_t bounds;
  // True when the aggregate has no elements and cannot execute an operation.
  bool empty;
} loom_execution_integer_facts_t;

static bool loom_execution_query_integer_facts(
    const loom_value_fact_table_t* fact_table, loom_type_t type,
    loom_value_id_t value_id, loom_execution_integer_facts_t* out_facts) {
  *out_facts = (loom_execution_integer_facts_t){0};
  if (loom_type_is_scalar(type)) {
    out_facts->bounds = loom_value_fact_table_lookup(fact_table, value_id);
    return true;
  }

  IREE_ASSERT(loom_type_is_vector(type));
  const uint64_t maximum_element_count =
      loom_value_fact_table_maximum_element_count(fact_table, type);
  if (maximum_element_count == 0) {
    out_facts->empty = true;
    return true;
  }

  int64_t lower_bound = 0;
  int64_t upper_bound = 0;
  if (!loom_value_facts_query_vector_integer_bounds(
          &fact_table->context,
          loom_value_fact_table_lookup(fact_table, value_id),
          maximum_element_count, &lower_bound, &upper_bound)) {
    return false;
  }
  if (lower_bound > upper_bound) {
    out_facts->empty = true;
    return true;
  }
  out_facts->bounds =
      loom_value_facts_make(lower_bound, upper_bound, /*known_divisor=*/1);
  return true;
}

static bool loom_execution_integer_shift_is_safe(
    const loom_module_t* module, const loom_op_t* op,
    const loom_value_fact_table_t* fact_table,
    loom_execution_condition_t condition) {
  const loom_type_t result_type =
      loom_module_value_type(module, loom_op_const_results(op)[0]);
  const loom_scalar_type_t element_type = loom_type_element_type(result_type);
  const bool is_index = element_type == LOOM_SCALAR_TYPE_INDEX;

  uint32_t bit_count = 0;
  if (is_index) {
    if (!fact_table->context.target_facts) {
      return false;
    }
    bit_count =
        fact_table->context.target_facts->storage.snapshot.index_bitwidth;
  } else {
    IREE_ASSERT(loom_scalar_type_is_integer(element_type));
    bit_count = (uint32_t)loom_scalar_type_bitwidth(element_type);
  }
  if (bit_count == 0 || bit_count > 64) {
    return false;
  }

  loom_execution_integer_facts_t count_facts;
  if (!loom_execution_query_integer_facts(fact_table, result_type,
                                          loom_op_const_operands(op)[1],
                                          &count_facts)) {
    return false;
  }
  if (count_facts.empty) {
    return true;
  }
  if (count_facts.bounds.range_lo < 0 ||
      (uint64_t)count_facts.bounds.range_hi >= bit_count) {
    return false;
  }
  if (condition == LOOM_EXECUTION_CONDITION_INTEGER_SHIFT_RIGHT) {
    return true;
  }

  const uint8_t maximum_shift = (uint8_t)count_facts.bounds.range_hi;
  if (maximum_shift == 0) {
    return true;
  }
  const uint8_t remaining_bit_count = (uint8_t)(bit_count - maximum_shift);

  if (!is_index && op->instance_flags == 0) {
    return true;
  }

  loom_execution_integer_facts_t input_facts;
  if (!loom_execution_query_integer_facts(fact_table, result_type,
                                          loom_op_const_operands(op)[0],
                                          &input_facts)) {
    return false;
  }
  if (input_facts.empty) {
    return true;
  }
  if (is_index) {
    return loom_value_facts_fit_signed_bit_count(input_facts.bounds,
                                                 remaining_bit_count);
  }

  if (iree_any_bit_set(op->instance_flags, LOOM_INTEGER_OVERFLOW_FLAG_NSW) &&
      !loom_value_facts_fit_signed_bit_count(input_facts.bounds,
                                             remaining_bit_count)) {
    return false;
  }
  if (iree_any_bit_set(op->instance_flags, LOOM_INTEGER_OVERFLOW_FLAG_NUW) &&
      !loom_value_facts_fit_unsigned_bit_count(input_facts.bounds,
                                               remaining_bit_count)) {
    return false;
  }
  return true;
}

bool loom_execution_condition_is_satisfied(
    const loom_module_t* module, const loom_op_t* op,
    const loom_value_fact_table_t* fact_table) {
  const loom_execution_condition_t condition =
      loom_op_semantics(module, op).execution_condition;
  if (condition == LOOM_EXECUTION_CONDITION_NONE || !fact_table) {
    return false;
  }
  IREE_ASSERT(!fact_table->has_conditioned_results);

  switch (condition) {
    case LOOM_EXECUTION_CONDITION_INTEGER_SHIFT_LEFT:
    case LOOM_EXECUTION_CONDITION_INTEGER_SHIFT_RIGHT:
      return loom_execution_integer_shift_is_safe(module, op, fact_table,
                                                  condition);
    default:
      IREE_ASSERT_UNREACHABLE("unimplemented execution condition");
      return false;
  }
}
