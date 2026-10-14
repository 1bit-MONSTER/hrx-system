// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Retained signed integer relations carried by scoped value aliases.

#ifndef LOOM_UTIL_FACT_RELATION_H_
#define LOOM_UTIL_FACT_RELATION_H_

#include "loom/analysis/integer_relation.h"
#include "loom/util/fact_table.h"

#ifdef __cplusplus
extern "C" {
#endif

// Returns the possible signed comparison outcomes for |lhs| relative to
// |rhs|. Identity, scalar integer facts, retained alias relations, and literal
// exclusions are resolved without walking producers or predicates. Unknown or
// contradictory retained state admits every outcome.
loom_integer_order_outcome_bits_t
loom_value_fact_table_query_signed_integer_order(
    const loom_value_fact_table_t* table, loom_value_id_t lhs,
    loom_value_id_t rhs);

// Returns the possible signed comparison outcomes for |value| relative to
// |literal|. Scalar integer facts and retained alias-local literal exclusions
// are resolved without materializing an SSA constant. Unknown or contradictory
// retained state admits every outcome.
loom_integer_order_outcome_bits_t
loom_value_fact_table_query_signed_integer_literal_order(
    const loom_value_fact_table_t* table, loom_value_id_t value,
    int64_t literal);

// Clears all scope-local retained relation state. The caller releases the
// transient arena after clearing the complete fact-table scope.
void loom_value_fact_integer_relations_clear_scope(
    loom_value_fact_table_t* table);

// Withdraws relations published for |value_id| before cyclic or incremental
// recomputation. The sparse entry remains reusable within the current scope.
void loom_value_fact_integer_relations_undefine(loom_value_fact_table_t* table,
                                                loom_value_id_t value_id);

// Rebuilds one selected alias's canonical relation sets in |target|.
iree_status_t loom_value_fact_integer_relations_clone_value(
    loom_value_fact_table_t* target, const loom_value_fact_table_t* source,
    loom_value_id_t value_id);

// Propagates inherited alias relations and retains signed predicates declared
// by a value-identity operation. All result plans are completed before any
// result relation is published.
iree_status_t loom_value_fact_integer_relations_propagate_origins(
    loom_value_fact_table_t* table, const loom_module_t* module,
    const loom_op_t* op, loom_trait_flags_t traits, bool* inout_changed);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_UTIL_FACT_RELATION_H_
