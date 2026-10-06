// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Immutable lexical composition of local and retained condition facts.

#ifndef LOOM_ANALYSIS_CONDITION_FACT_SCOPE_H_
#define LOOM_ANALYSIS_CONDITION_FACT_SCOPE_H_

#include "iree/base/api.h"
#include "loom/analysis/cfg_condition_facts.h"
#include "loom/analysis/condition_edge_projection.h"

#ifdef __cplusplus
extern "C" {
#endif

// One immutable fact fragment extending a parent lexical scope.
typedef struct loom_condition_fact_scope_t {
  // Outer lexical facts, or NULL at the root.
  const struct loom_condition_fact_scope_t* parent;

  // Complete local derivation, or NULL for an indexed fragment.
  const loom_condition_derivation_t* local_derivation;

  // Structured-edge projection, or NULL for another fragment kind.
  const loom_condition_edge_projection_t* edge_projection;

  // Retained CFG relation table, or NULL for a local fragment.
  const loom_cfg_condition_relation_table_t* relation_table;

  // View owned by relation_table when this is an indexed fragment.
  const loom_cfg_condition_relation_view_t* relation_view;
} loom_condition_fact_scope_t;

// Initializes a caller-owned scope node extending |parent| with one complete
// local derivation. All inputs must outlive the node.
void loom_condition_fact_scope_initialize_local(
    const loom_condition_fact_scope_t* parent,
    const loom_condition_derivation_t* derivation,
    loom_condition_fact_scope_t* out_scope);

// Initializes a caller-owned scope node extending |parent| with one compact
// structured-edge projection. All inputs must outlive the node.
void loom_condition_fact_scope_initialize_projected(
    const loom_condition_fact_scope_t* parent,
    const loom_condition_edge_projection_t* projection,
    loom_condition_fact_scope_t* out_scope);

// Extends |parent| with the retained structured-edge projection entering
// |region|. Returns |parent| unchanged when no observable projection exists;
// otherwise allocates one scope node in |arena|.
iree_status_t loom_condition_fact_scope_extend_region(
    const loom_value_fact_table_t* fact_table, const loom_region_t* region,
    const loom_condition_fact_scope_t* parent, iree_arena_allocator_t* arena,
    const loom_condition_fact_scope_t** out_scope);

// Initializes a caller-owned scope node extending |parent| with one retained
// indexed view. The table and view must outlive the node.
void loom_condition_fact_scope_initialize_indexed(
    const loom_condition_fact_scope_t* parent,
    const loom_cfg_condition_relation_table_t* table,
    const loom_cfg_condition_relation_view_t* view,
    loom_condition_fact_scope_t* out_scope);

// Returns true when any scope fragment contains integer relations.
bool loom_condition_fact_scope_has_integer_relations(
    const loom_condition_fact_scope_t* scope);

// Applies all anchored relations in |scope| to scalar facts for |value_id|.
bool loom_condition_fact_scope_apply_to_value_facts(
    const loom_condition_fact_scope_t* scope,
    const loom_value_fact_table_t* fact_table, loom_value_id_t value_id,
    loom_value_facts_t* inout_facts);

// Attempts to evaluate |queried| from the conjunction of every fragment in
// |scope|. Contradictory fragments leave the result unknown.
bool loom_condition_fact_scope_proves_integer_relation(
    const loom_condition_fact_scope_t* scope,
    const loom_value_fact_table_t* fact_table,
    const loom_condition_integer_relation_t* queried, bool* out_result);

// Attempts to prove exact Boolean truth through local and indexed fragments,
// including composition through supported Boolean producers.
iree_status_t loom_condition_fact_scope_proves_condition(
    loom_condition_query_t* query, const loom_value_fact_table_t* fact_table,
    const loom_condition_fact_scope_t* scope, loom_value_id_t condition_value,
    bool* out_condition, bool* out_proven);

// Visits every local or projected relation once and retained CFG relations
// incident to any of |anchors| in lexical scope order. Repeated retained CFG
// relations across anchors may be visited more than once. Returns false when
// |visit| stops.
bool loom_condition_fact_scope_for_each_anchored_while(
    const loom_condition_fact_scope_t* scope,
    const loom_value_fact_table_t* fact_table,
    const loom_condition_integer_operand_t* anchors,
    iree_host_size_t anchor_count, loom_cfg_condition_relation_visit_fn_t visit,
    void* user_data);

// Visits every local or projected relation once and retained CFG relations
// incident to any value in |value_ids|. Repeated retained CFG relations across
// values may be visited more than once. Returns false when |visit| stops.
bool loom_condition_fact_scope_for_each_value_anchored_while(
    const loom_condition_fact_scope_t* scope,
    const loom_value_fact_table_t* fact_table, const loom_value_id_t* value_ids,
    iree_host_size_t value_count, loom_cfg_condition_relation_visit_fn_t visit,
    void* user_data);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_ANALYSIS_CONDITION_FACT_SCOPE_H_
