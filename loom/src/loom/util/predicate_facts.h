// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_UTIL_PREDICATE_FACTS_H_
#define LOOM_UTIL_PREDICATE_FACTS_H_

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
