// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Function-local selection of disjoint source-to-Low rule graphs.

#ifndef LOOM_CODEGEN_LOW_LOWER_SOURCE_GRAPH_PLAN_H_
#define LOOM_CODEGEN_LOW_LOWER_SOURCE_GRAPH_PLAN_H_

#include "iree/base/api.h"
#include "iree/base/internal/arena.h"
#include "loom/codegen/low/lower/rules.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct loom_low_lower_source_graph_plan_t
    loom_low_lower_source_graph_plan_t;
typedef struct loom_low_lower_source_graph_candidate_t
    loom_low_lower_source_graph_candidate_t;
typedef struct loom_low_lower_source_graph_alternative_t
    loom_low_lower_source_graph_alternative_t;

// One exclusive SSA edge in a source graph. Node ordinals index the
// root-first |node_indices| passed with the candidate.
typedef struct loom_low_lower_source_graph_edge_t {
  // Node defining the exclusive connection value.
  uint8_t definition_node_ordinal;
  // Node using the exclusive connection value.
  uint8_t user_node_ordinal;
} loom_low_lower_source_graph_edge_t;

// Materializes the definition-to-use edges encoded by a generated rule.
// |out_edges| must provide one entry per related source node; singleton rules
// write nothing.
void loom_low_lower_source_graph_rule_edges(
    const loom_low_lower_rule_set_t* rule_set,
    const loom_low_lower_rule_t* rule,
    loom_low_lower_source_graph_edge_t* out_edges);

typedef enum loom_low_lower_source_graph_alternative_state_e {
  // Alternative cannot participate in the current resolution.
  LOOM_LOW_LOWER_SOURCE_GRAPH_ALTERNATIVE_DISABLED = 0,
  // Alternative participates only when its graph has no enabled alternative.
  // Covers prefer enabled nodes before authored priority, so a fallback fusion
  // cannot displace an enabled decomposition.
  LOOM_LOW_LOWER_SOURCE_GRAPH_ALTERNATIVE_FALLBACK = 1,
  // Alternative is immediately selectable for its graph.
  LOOM_LOW_LOWER_SOURCE_GRAPH_ALTERNATIVE_ENABLED = 2,
  // Singleton alternative owns its node independently of graph priority.
  LOOM_LOW_LOWER_SOURCE_GRAPH_ALTERNATIVE_FIXED = 3,
} loom_low_lower_source_graph_alternative_state_t;

// Returns the current selection state of |alternative|.
typedef loom_low_lower_source_graph_alternative_state_t (
    *loom_low_lower_source_graph_alternative_query_fn_t)(
    void* user_data,
    const loom_low_lower_source_graph_alternative_t* alternative);

typedef struct loom_low_lower_source_graph_alternative_query_callback_t {
  // Infallible alternative query, or NULL when every alternative is enabled.
  loom_low_lower_source_graph_alternative_query_fn_t fn;
  // Caller-owned payload passed to |fn|.
  void* user_data;
} loom_low_lower_source_graph_alternative_query_callback_t;

// Allocates an empty source graph plan owned by |arena|.
iree_status_t loom_low_lower_source_graph_plan_allocate(
    iree_arena_allocator_t* arena,
    loom_low_lower_source_graph_plan_t** out_plan);

// Adds one alternative over |node_indices| in root-first order.
//
// Node indices are caller-defined dense function-local identities. Multi-node
// candidates provide their defining SSA tree edges; singleton candidates pass
// NULL. The union of matched edges is a forest because each definition exposes
// at most one graph-connected result and each connection has one user.
// |user_data| is returned unchanged by the alternative accessor and must
// outlive the plan. Exact graphs share one ownership candidate and retain
// alternative insertion order. Graph candidates retain insertion order for
// equal-priority selection.
iree_status_t loom_low_lower_source_graph_plan_add_alternative(
    loom_low_lower_source_graph_plan_t* plan, const uint32_t* node_indices,
    uint8_t node_count, const loom_low_lower_source_graph_edge_t* edges,
    uint8_t priority, void* user_data,
    loom_low_lower_source_graph_alternative_t** out_alternative);

// Selects a disjoint candidate cover for every participating node.
//
// Forest dynamic programming selects the exact disjoint cover maximizing, in
// order, required nodes covered, nodes covered by enabled alternatives, and
// authored priority per covered node. Every other node has an active singleton,
// so covering the maximum required set also covers every optional node.
// Complete coverage dominates availability, which dominates optimization
// preference. A graph rooted at a node wins an equal score over the decomposed
// definition subtrees, maximizing fusion locally. Candidates rooted at the
// same node retain insertion order.
//
// For each exact graph, the first fixed alternative wins, followed by the first
// enabled alternative; the first fallback is used only when neither exists. A
// fixed alternative must be a singleton and excludes every overlapping graph.
// Repeated calls reuse the same indexed storage and permit |query| to
// invalidate alternatives monotonically during plan refinement. |out_complete|
// is false only when no active cover exists for every participating node.
iree_status_t loom_low_lower_source_graph_plan_resolve(
    loom_low_lower_source_graph_plan_t* plan,
    loom_low_lower_source_graph_alternative_query_callback_t query,
    bool* out_complete);

// Returns the selected alternative covering |node_index|, or NULL when the
// node does not participate or no active candidate covers it.
const loom_low_lower_source_graph_alternative_t*
loom_low_lower_source_graph_plan_selected_alternative(
    const loom_low_lower_source_graph_plan_t* plan, uint32_t node_index);

// Returns the caller-owned payload retained for |alternative|.
void* loom_low_lower_source_graph_alternative_user_data(
    const loom_low_lower_source_graph_alternative_t* alternative);

// Returns the exact source graph shared by |alternative|.
const loom_low_lower_source_graph_candidate_t*
loom_low_lower_source_graph_alternative_candidate(
    const loom_low_lower_source_graph_alternative_t* alternative);

// Returns graph-level caller data shared by all exact alternatives.
void* loom_low_lower_source_graph_candidate_user_data(
    const loom_low_lower_source_graph_candidate_t* candidate);

// Assigns graph-level caller data once for |alternative|'s exact graph.
void loom_low_lower_source_graph_alternative_set_candidate_user_data(
    const loom_low_lower_source_graph_alternative_t* alternative,
    void* user_data);

// Returns the number of nodes covered by |candidate|.
uint8_t loom_low_lower_source_graph_candidate_node_count(
    const loom_low_lower_source_graph_candidate_t* candidate);

// Returns one caller-defined node index from |candidate|.
uint32_t loom_low_lower_source_graph_candidate_node(
    const loom_low_lower_source_graph_candidate_t* candidate,
    uint8_t node_ordinal);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_CODEGEN_LOW_LOWER_SOURCE_GRAPH_PLAN_H_
