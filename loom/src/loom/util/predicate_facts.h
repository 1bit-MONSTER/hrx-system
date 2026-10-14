// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_UTIL_PREDICATE_FACTS_H_
#define LOOM_UTIL_PREDICATE_FACTS_H_

#include "loom/decision/predicate.h"
#include "loom/util/fact_table.h"

#ifdef __cplusplus
extern "C" {
#endif

// Exports directly representable refinements from |proven_facts| over
// |inferred_facts| as predicates on |value|. A tighter exact or interval fact
// contributes one equality or range predicate. Stronger divisibility
// contributes one multiple-of predicate when the value is not exact. Unknown,
// floating-point, and already-inferred facts contribute nothing. Returns the
// number of predicates written, bounded by |capacity|.
uint16_t loom_value_facts_export_integer_refinements(
    loom_value_id_t value, loom_value_facts_t inferred_facts,
    loom_value_facts_t proven_facts, loom_predicate_t* predicates,
    uint16_t capacity);

// Proves one verified predicate from the scalar, identity, and retained
// relation domains in |table|. The query is allocation-free and does not walk
// IR or producer chains. A NULL table proves only literal and direct-identity
// predicates.
loom_decision_truth_t loom_value_fact_table_evaluate_predicate(
    const loom_value_fact_table_t* table, const loom_predicate_t* predicate);

// Returns true when every predicate in the verified list is proven true by
// |table|. An empty conjunction is true.
bool loom_value_fact_table_proves_predicate_list(
    const loom_value_fact_table_t* table, const loom_predicate_t* predicates,
    uint16_t predicate_count);

// Returns true when one verified |premise| predicate logically establishes
// the verified |conclusion| predicate. SSA arguments compare by their retained
// canonical identities in |table|; no numeric, contextual, or path facts are
// consulted. This makes the query suitable for deciding whether one explicit
// predicate carrier subsumes another without confusing ambient proof with a
// durable program contract.
bool loom_value_fact_table_predicate_implies(
    const loom_value_fact_table_t* table, const loom_predicate_t* premise,
    const loom_predicate_t* conclusion);

// Returns true when an explicit predicate on the verified pure fact-identity
// |carrier| establishes |conclusion|. Other operations return false. Only the
// carrier's declared predicates and retained SSA identities participate;
// ambient numeric and path facts cannot substitute for this durable program
// contract. The query does not follow the carrier's operands or allocate
// storage.
bool loom_value_fact_table_carrier_implies_predicate(
    const loom_value_fact_table_t* table, const loom_module_t* module,
    const loom_op_t* carrier, const loom_predicate_t* conclusion);

// Tightens one fact per SSA alias using predicates over those aliases and
// externally referenced SSA values already defined in |table|. Literal
// predicates refine their target alias directly. Binary relational predicates
// refine aliases from the known interval of their counterpart without
// modifying external facts. |inout_facts[i]| corresponds to |values[i]|.
//
// Small lists use bounded linear scans without allocating. Larger lists use a
// reusable direct-address ordinal map owned by |table|. Each map capacity is
// allocated and zeroed once; calls then populate and clear only current
// aliases, making predicate application O(value_count + predicate_count). The
// map's high-water storage is two bytes per addressable SSA value ID and is
// released with the table's transient scope. Returns an error only when growing
// that table-owned scratch map fails.
iree_status_t loom_value_fact_table_apply_alias_predicates(
    loom_value_fact_table_t* table, const loom_value_id_t* values,
    uint16_t value_count, const loom_predicate_t* predicates,
    uint16_t predicate_count, loom_value_facts_t* inout_facts);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_UTIL_PREDICATE_FACTS_H_
