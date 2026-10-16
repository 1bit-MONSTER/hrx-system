// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/codegen/low/lower/source_graph_plan.h"

#include <string.h>

#define LOOM_LOW_LOWER_SOURCE_GRAPH_NODE_INVALID UINT32_MAX
#define LOOM_LOW_LOWER_SOURCE_GRAPH_NODE_COUNT_MAX \
  (UINT32_MAX / LOOM_LOW_LOWER_RULE_PRIORITY_MAX)

struct loom_low_lower_source_graph_alternative_t {
  // Next alternative for the exact same source graph.
  loom_low_lower_source_graph_alternative_t* next;
  // Exact source graph shared with sibling alternatives.
  loom_low_lower_source_graph_candidate_t* candidate;
  // Caller-owned alternative payload.
  void* user_data;
  // Authored selection priority when this alternative is active.
  uint8_t priority;
};

struct loom_low_lower_source_graph_candidate_t {
  // Next candidate in stable insertion order.
  loom_low_lower_source_graph_candidate_t* next;
  union {
    // Exact-graph interning state used only during collection.
    struct {
      // Next candidate in the exact-graph intern bucket.
      loom_low_lower_source_graph_candidate_t* next;
      // Hash of ordered node identities and definition-to-use edges.
      uint64_t hash;
    } collection;
    // Selection state used only after collection.
    struct {
      // Next candidate with the same graph-tree top.
      loom_low_lower_source_graph_candidate_t* next;
      // Alternative selected for the current resolution.
      const loom_low_lower_source_graph_alternative_t* alternative;
    } selection;
  } phase;
  // First alternative in stable insertion order.
  loom_low_lower_source_graph_alternative_t* first_alternative;
  // Last alternative in stable insertion order.
  loom_low_lower_source_graph_alternative_t* last_alternative;
  // Caller-owned payload shared by exact graph alternatives.
  void* user_data;
  // Graph-tree node nearest the component root.
  uint32_t top_node_index;
  // Number of trailing node_indices entries.
  uint8_t node_count;
  // True when the selected alternative is enabled or fixed, not a fallback.
  bool selected_enabled;
  // Caller-defined identities for every covered node, with the rule root first.
  // The node_count - 1 edges immediately follow this array.
  uint32_t node_indices[];
};

// Candidate admission bounds every component below UINT32_MAX.
typedef struct loom_low_lower_source_graph_score_t {
  // Covered nodes without an active singleton alternative.
  uint32_t required_count;
  // Covered nodes whose selected alternative does not require a fallback.
  uint32_t enabled_count;
  // Sum of authored priority over covered nodes.
  uint32_t priority;
} loom_low_lower_source_graph_score_t;

static bool loom_low_lower_source_graph_score_greater_equal(
    loom_low_lower_source_graph_score_t lhs,
    loom_low_lower_source_graph_score_t rhs) {
  if (lhs.required_count != rhs.required_count) {
    return lhs.required_count > rhs.required_count;
  }
  if (lhs.enabled_count != rhs.enabled_count) {
    return lhs.enabled_count > rhs.enabled_count;
  }
  return lhs.priority >= rhs.priority;
}

struct loom_low_lower_source_graph_plan_t {
  // Arena owning this plan and every candidate.
  iree_arena_allocator_t* arena;
  // First candidate in stable insertion order.
  loom_low_lower_source_graph_candidate_t* first_candidate;
  // Last candidate in stable insertion order.
  loom_low_lower_source_graph_candidate_t* last_candidate;
  // Exact-graph intern buckets used only during collection.
  loom_low_lower_source_graph_candidate_t** candidate_buckets;
  // Power-of-two number of exact-graph intern buckets.
  iree_host_size_t candidate_bucket_capacity;
  // Number of retained graph candidates.
  iree_host_size_t candidate_count;
  // First candidate indexed by its graph-tree top node during resolution, then
  // the selected candidate indexed by node identity after reconstruction.
  loom_low_lower_source_graph_candidate_t** top_candidates;
  // Unique graph-connected user indexed by definition node.
  uint32_t* tree_parents;
  // Active singleton state during scoring, then selected-node marks.
  uint8_t* singleton_nodes;
  // Sum of solved child-subtree scores indexed by graph-tree node.
  loom_low_lower_source_graph_score_t* baseline_scores;
  // Best solved score indexed by graph-tree node.
  loom_low_lower_source_graph_score_t* best_scores;
  // Bottom-up graph-tree order, reused in reverse for reconstruction.
  uint32_t* traversal_queue;
  // Number of participating nodes in |traversal_queue|.
  iree_host_size_t traversal_count;
  // Number of entries in every node-indexed array.
  iree_host_size_t node_capacity;
  // True after indexed resolution storage and tree topology are available.
  bool storage_allocated;
};

void loom_low_lower_source_graph_rule_edges(
    const loom_low_lower_rule_set_t* rule_set,
    const loom_low_lower_rule_t* rule,
    loom_low_lower_source_graph_edge_t* out_edges) {
  const uint16_t source_node_start =
      loom_low_lower_rule_source_node_start(rule);
  const uint8_t source_node_count = loom_low_lower_rule_source_node_count(rule);
  for (uint8_t i = 0; i < source_node_count; ++i) {
    const loom_low_lower_source_node_t* source_node =
        &rule_set->source_nodes[source_node_start + i];
    const uint8_t node_ordinal = (uint8_t)(i + 1);
    switch (source_node->relation) {
      case LOOM_LOW_LOWER_SOURCE_NODE_ADJACENT_UNIQUE_USER:
      case LOOM_LOW_LOWER_SOURCE_NODE_EXCLUSIVE_USER:
        out_edges[i] = (loom_low_lower_source_graph_edge_t){
            .definition_node_ordinal = source_node->parent_node_index,
            .user_node_ordinal = node_ordinal,
        };
        break;
      case LOOM_LOW_LOWER_SOURCE_NODE_ADJACENT_DEFINITION:
      case LOOM_LOW_LOWER_SOURCE_NODE_EXCLUSIVE_DEFINITION:
        out_edges[i] = (loom_low_lower_source_graph_edge_t){
            .definition_node_ordinal = node_ordinal,
            .user_node_ordinal = source_node->parent_node_index,
        };
        break;
      default:
        IREE_ASSERT_UNREACHABLE("unknown generated source-node relation");
        IREE_BUILTIN_UNREACHABLE();
    }
  }
}

static const loom_low_lower_source_graph_edge_t*
loom_low_lower_source_graph_candidate_edges(
    const loom_low_lower_source_graph_candidate_t* candidate) {
  return (const loom_low_lower_source_graph_edge_t*)(candidate->node_indices +
                                                     candidate->node_count);
}

static loom_low_lower_source_graph_edge_t*
loom_low_lower_source_graph_candidate_mutable_edges(
    loom_low_lower_source_graph_candidate_t* candidate) {
  return (loom_low_lower_source_graph_edge_t*)(candidate->node_indices +
                                               candidate->node_count);
}

static uint64_t loom_low_lower_source_graph_hash(
    const uint32_t* node_indices, uint8_t node_count,
    const loom_low_lower_source_graph_edge_t* edges) {
  uint64_t hash = UINT64_C(0xcbf29ce484222325);
  for (uint8_t i = 0; i < node_count; ++i) {
    hash ^= node_indices[i];
    hash *= UINT64_C(0x100000001b3);
  }
  for (uint8_t i = 1; i < node_count; ++i) {
    hash ^= edges[i - 1].definition_node_ordinal;
    hash *= UINT64_C(0x100000001b3);
    hash ^= edges[i - 1].user_node_ordinal;
    hash *= UINT64_C(0x100000001b3);
  }
  hash ^= node_count;
  return hash;
}

static bool loom_low_lower_source_graph_candidate_matches(
    const loom_low_lower_source_graph_candidate_t* candidate,
    const uint32_t* node_indices, uint8_t node_count,
    const loom_low_lower_source_graph_edge_t* edges, uint64_t graph_hash) {
  if (candidate->phase.collection.hash != graph_hash ||
      candidate->node_count != node_count ||
      memcmp(candidate->node_indices, node_indices,
             (iree_host_size_t)node_count * sizeof(*node_indices)) != 0) {
    return false;
  }
  return node_count == 1 ||
         memcmp(loom_low_lower_source_graph_candidate_edges(candidate), edges,
                (iree_host_size_t)(node_count - 1) * sizeof(*edges)) == 0;
}

static bool loom_low_lower_source_graph_candidate_contains(
    const loom_low_lower_source_graph_candidate_t* candidate,
    uint32_t node_index) {
  for (uint8_t i = 0; i < candidate->node_count; ++i) {
    if (candidate->node_indices[i] == node_index) {
      return true;
    }
  }
  return false;
}

static iree_status_t loom_low_lower_source_graph_plan_reserve_buckets(
    loom_low_lower_source_graph_plan_t* plan, iree_host_size_t capacity) {
  if (plan->candidate_bucket_capacity >= capacity) {
    return iree_ok_status();
  }
  iree_host_size_t new_capacity = plan->candidate_bucket_capacity == 0
                                      ? 16
                                      : plan->candidate_bucket_capacity;
  while (new_capacity < capacity) {
    if (new_capacity > IREE_HOST_SIZE_MAX / 2) {
      return iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                              "source graph candidate table overflow");
    }
    new_capacity *= 2;
  }
  loom_low_lower_source_graph_candidate_t** new_buckets = NULL;
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      plan->arena, new_capacity, sizeof(*new_buckets), (void**)&new_buckets));
  memset(new_buckets, 0, new_capacity * sizeof(*new_buckets));
  const iree_host_size_t mask = new_capacity - 1;
  for (loom_low_lower_source_graph_candidate_t* candidate =
           plan->first_candidate;
       candidate != NULL; candidate = candidate->next) {
    const iree_host_size_t bucket = candidate->phase.collection.hash & mask;
    candidate->phase.collection.next = new_buckets[bucket];
    new_buckets[bucket] = candidate;
  }
  plan->candidate_buckets = new_buckets;
  plan->candidate_bucket_capacity = new_capacity;
  return iree_ok_status();
}

iree_status_t loom_low_lower_source_graph_plan_allocate(
    iree_arena_allocator_t* arena,
    loom_low_lower_source_graph_plan_t** out_plan) {
  *out_plan = NULL;
  loom_low_lower_source_graph_plan_t* plan = NULL;
  IREE_RETURN_IF_ERROR(
      iree_arena_allocate(arena, sizeof(*plan), (void**)&plan));
  memset(plan, 0, sizeof(*plan));
  plan->arena = arena;
  *out_plan = plan;
  return iree_ok_status();
}

static iree_status_t loom_low_lower_source_graph_plan_add_candidate(
    loom_low_lower_source_graph_plan_t* plan, const uint32_t* node_indices,
    uint8_t node_count, const loom_low_lower_source_graph_edge_t* edges,
    uint64_t graph_hash,
    loom_low_lower_source_graph_candidate_t** out_candidate) {
  *out_candidate = NULL;
  IREE_ASSERT(!plan->storage_allocated,
              "source graph candidates cannot be added after resolution");
  IREE_ASSERT_GT(node_count, 0);
  if (plan->candidate_count >= IREE_HOST_SIZE_MAX / 2) {
    return iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                            "source graph candidate count overflow");
  }
  const iree_host_size_t required_bucket_capacity =
      plan->candidate_count == 0 ? 16 : (plan->candidate_count + 1) * 2;
  IREE_RETURN_IF_ERROR(loom_low_lower_source_graph_plan_reserve_buckets(
      plan, required_bucket_capacity));
  const iree_host_size_t edge_count = node_count - 1;
  const iree_host_size_t allocation_size =
      sizeof(loom_low_lower_source_graph_candidate_t) +
      (iree_host_size_t)node_count * sizeof(*node_indices) +
      edge_count * sizeof(*edges);
  loom_low_lower_source_graph_candidate_t* candidate = NULL;
  IREE_RETURN_IF_ERROR(
      iree_arena_allocate(plan->arena, allocation_size, (void**)&candidate));
  *candidate = (loom_low_lower_source_graph_candidate_t){
      .phase.collection.hash = graph_hash,
      .top_node_index = LOOM_LOW_LOWER_SOURCE_GRAPH_NODE_INVALID,
      .node_count = node_count,
  };
  memcpy(candidate->node_indices, node_indices,
         (iree_host_size_t)node_count * sizeof(*node_indices));
  if (edge_count != 0) {
    memcpy(loom_low_lower_source_graph_candidate_mutable_edges(candidate),
           edges, edge_count * sizeof(*edges));
  }
  for (uint8_t i = 0; i < node_count; ++i) {
    const iree_host_size_t required_capacity =
        (iree_host_size_t)node_indices[i] + 1;
    if (required_capacity > plan->node_capacity) {
      plan->node_capacity = required_capacity;
    }
  }
  if (plan->last_candidate != NULL) {
    plan->last_candidate->next = candidate;
  } else {
    plan->first_candidate = candidate;
  }
  plan->last_candidate = candidate;
  const iree_host_size_t bucket =
      graph_hash & (plan->candidate_bucket_capacity - 1);
  candidate->phase.collection.next = plan->candidate_buckets[bucket];
  plan->candidate_buckets[bucket] = candidate;
  ++plan->candidate_count;
  *out_candidate = candidate;
  return iree_ok_status();
}

static iree_status_t loom_low_lower_source_graph_plan_intern_candidate(
    loom_low_lower_source_graph_plan_t* plan, const uint32_t* node_indices,
    uint8_t node_count, const loom_low_lower_source_graph_edge_t* edges,
    loom_low_lower_source_graph_candidate_t** out_candidate) {
  *out_candidate = NULL;
  IREE_ASSERT(!plan->storage_allocated,
              "source graph candidates cannot be added after resolution");
  IREE_ASSERT_GT(node_count, 0);
  const uint64_t graph_hash =
      loom_low_lower_source_graph_hash(node_indices, node_count, edges);
  if (plan->candidate_bucket_capacity != 0) {
    const iree_host_size_t bucket =
        graph_hash & (plan->candidate_bucket_capacity - 1);
    for (loom_low_lower_source_graph_candidate_t* candidate =
             plan->candidate_buckets[bucket];
         candidate != NULL; candidate = candidate->phase.collection.next) {
      if (loom_low_lower_source_graph_candidate_matches(
              candidate, node_indices, node_count, edges, graph_hash)) {
        *out_candidate = candidate;
        return iree_ok_status();
      }
    }
  }
  return loom_low_lower_source_graph_plan_add_candidate(
      plan, node_indices, node_count, edges, graph_hash, out_candidate);
}

iree_status_t loom_low_lower_source_graph_plan_add_alternative(
    loom_low_lower_source_graph_plan_t* plan, const uint32_t* node_indices,
    uint8_t node_count, const loom_low_lower_source_graph_edge_t* edges,
    uint8_t priority, void* user_data,
    loom_low_lower_source_graph_alternative_t** out_alternative) {
  if (out_alternative != NULL) {
    *out_alternative = NULL;
  }
  IREE_ASSERT_GT(node_count, 0);
  IREE_ASSERT_LE(node_count, LOOM_LOW_LOWER_MAX_SOURCE_NODES);
  IREE_ASSERT_LE(priority, LOOM_LOW_LOWER_RULE_PRIORITY_MAX);
  IREE_ASSERT((node_count == 1) == (edges == NULL));
  for (uint8_t i = 0; i < node_count; ++i) {
    IREE_ASSERT_NE(node_indices[i], LOOM_LOW_LOWER_SOURCE_GRAPH_NODE_INVALID);
    if (node_indices[i] >= LOOM_LOW_LOWER_SOURCE_GRAPH_NODE_COUNT_MAX) {
      return iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                              "source candidate node count overflow");
    }
  }
  for (uint8_t i = 1; i < node_count; ++i) {
    IREE_ASSERT_LT(edges[i - 1].definition_node_ordinal, node_count);
    IREE_ASSERT_LT(edges[i - 1].user_node_ordinal, node_count);
    IREE_ASSERT_NE(edges[i - 1].definition_node_ordinal,
                   edges[i - 1].user_node_ordinal);
  }
  loom_low_lower_source_graph_candidate_t* candidate = NULL;
  IREE_RETURN_IF_ERROR(loom_low_lower_source_graph_plan_intern_candidate(
      plan, node_indices, node_count, edges, &candidate));
  loom_low_lower_source_graph_alternative_t* alternative = NULL;
  IREE_RETURN_IF_ERROR(iree_arena_allocate(plan->arena, sizeof(*alternative),
                                           (void**)&alternative));
  *alternative = (loom_low_lower_source_graph_alternative_t){
      .candidate = candidate,
      .user_data = user_data,
      .priority = priority,
  };
  if (candidate->last_alternative != NULL) {
    candidate->last_alternative->next = alternative;
  } else {
    candidate->first_alternative = alternative;
  }
  candidate->last_alternative = alternative;
  if (out_alternative != NULL) {
    *out_alternative = alternative;
  }
  return iree_ok_status();
}

static iree_status_t loom_low_lower_source_graph_plan_allocate_storage(
    loom_low_lower_source_graph_plan_t* plan) {
  if (plan->storage_allocated) {
    return iree_ok_status();
  }
  if (plan->node_capacity == 0) {
    plan->storage_allocated = true;
    return iree_ok_status();
  }
  const iree_host_size_t node_capacity = plan->node_capacity;
  const iree_host_size_t node_storage_size =
      sizeof(*plan->top_candidates) + sizeof(*plan->baseline_scores) +
      sizeof(*plan->best_scores) + sizeof(*plan->tree_parents) +
      sizeof(*plan->traversal_queue) + sizeof(*plan->singleton_nodes);
  uint8_t* node_storage = NULL;
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      plan->arena, node_capacity, node_storage_size, (void**)&node_storage));
  plan->top_candidates =
      (loom_low_lower_source_graph_candidate_t**)node_storage;
  node_storage += node_capacity * sizeof(*plan->top_candidates);
  plan->baseline_scores = (loom_low_lower_source_graph_score_t*)node_storage;
  node_storage += node_capacity * sizeof(*plan->baseline_scores);
  plan->best_scores = (loom_low_lower_source_graph_score_t*)node_storage;
  node_storage += node_capacity * sizeof(*plan->best_scores);
  plan->tree_parents = (uint32_t*)node_storage;
  node_storage += node_capacity * sizeof(*plan->tree_parents);
  plan->traversal_queue = (uint32_t*)node_storage;
  node_storage += node_capacity * sizeof(*plan->traversal_queue);
  plan->singleton_nodes = node_storage;

  memset(plan->tree_parents, 0xFF, node_capacity * sizeof(*plan->tree_parents));
  uint32_t* pending_child_counts = (uint32_t*)plan->top_candidates;
  memset(pending_child_counts, 0xFF,
         node_capacity * sizeof(*pending_child_counts));
  iree_host_size_t active_node_count = 0;
  for (loom_low_lower_source_graph_candidate_t* candidate =
           plan->first_candidate;
       candidate != NULL; candidate = candidate->next) {
    for (uint8_t i = 0; i < candidate->node_count; ++i) {
      const uint32_t node_index = candidate->node_indices[i];
      if (pending_child_counts[node_index] ==
          LOOM_LOW_LOWER_SOURCE_GRAPH_NODE_INVALID) {
        pending_child_counts[node_index] = 0;
        ++active_node_count;
      }
    }
    const loom_low_lower_source_graph_edge_t* edges =
        loom_low_lower_source_graph_candidate_edges(candidate);
    for (uint8_t i = 1; i < candidate->node_count; ++i) {
      const uint32_t definition_node =
          candidate->node_indices[edges[i - 1].definition_node_ordinal];
      const uint32_t user_node =
          candidate->node_indices[edges[i - 1].user_node_ordinal];
      uint32_t* tree_parent = &plan->tree_parents[definition_node];
      if (*tree_parent == LOOM_LOW_LOWER_SOURCE_GRAPH_NODE_INVALID) {
        *tree_parent = user_node;
      } else {
        IREE_ASSERT_EQ(*tree_parent, user_node,
                       "source graph definition has multiple graph users");
      }
    }
  }

  for (loom_low_lower_source_graph_candidate_t* candidate =
           plan->first_candidate;
       candidate != NULL; candidate = candidate->next) {
    uint8_t top_count = 0;
    for (uint8_t i = 0; i < candidate->node_count; ++i) {
      const uint32_t node_index = candidate->node_indices[i];
      const uint32_t tree_parent = plan->tree_parents[node_index];
      if (tree_parent == LOOM_LOW_LOWER_SOURCE_GRAPH_NODE_INVALID ||
          !loom_low_lower_source_graph_candidate_contains(candidate,
                                                          tree_parent)) {
        candidate->top_node_index = node_index;
        ++top_count;
      }
    }
    IREE_ASSERT_EQ(top_count, 1,
                   "source graph candidate must be a connected tree");
  }

  // Compute the immutable bottom-up forest order once. The selected-candidate
  // array is still unused and provides temporary child counts without retaining
  // another node-indexed allocation for every refinement.
  for (iree_host_size_t i = 0; i < node_capacity; ++i) {
    if (pending_child_counts[i] == LOOM_LOW_LOWER_SOURCE_GRAPH_NODE_INVALID) {
      continue;
    }
    const uint32_t tree_parent = plan->tree_parents[i];
    if (tree_parent != LOOM_LOW_LOWER_SOURCE_GRAPH_NODE_INVALID) {
      ++pending_child_counts[tree_parent];
    }
  }
  iree_host_size_t queue_head = 0;
  iree_host_size_t queue_tail = 0;
  for (iree_host_size_t i = 0; i < node_capacity; ++i) {
    if (pending_child_counts[i] == 0) {
      plan->traversal_queue[queue_tail++] = (uint32_t)i;
    }
  }
  while (queue_head < queue_tail) {
    const uint32_t node_index = plan->traversal_queue[queue_head++];
    const uint32_t tree_parent = plan->tree_parents[node_index];
    if (tree_parent != LOOM_LOW_LOWER_SOURCE_GRAPH_NODE_INVALID) {
      IREE_ASSERT_GT(pending_child_counts[tree_parent], 0);
      if (--pending_child_counts[tree_parent] == 0) {
        plan->traversal_queue[queue_tail++] = tree_parent;
      }
    }
  }
  IREE_ASSERT_EQ(queue_tail, active_node_count,
                 "source graph relation topology must be acyclic");
  plan->traversal_count = queue_tail;

  plan->storage_allocated = true;
  return iree_ok_status();
}

static loom_low_lower_source_graph_score_t
loom_low_lower_source_graph_candidate_score(
    const loom_low_lower_source_graph_plan_t* plan,
    const loom_low_lower_source_graph_candidate_t* candidate) {
  loom_low_lower_source_graph_score_t score = {
      .enabled_count = candidate->selected_enabled ? candidate->node_count : 0,
      .priority = (uint32_t)candidate->phase.selection.alternative->priority *
                  candidate->node_count,
  };
  for (uint8_t i = 0; i < candidate->node_count; ++i) {
    const uint32_t node_index = candidate->node_indices[i];
    if (!plan->singleton_nodes[node_index]) {
      ++score.required_count;
    }
    score.required_count += plan->baseline_scores[node_index].required_count;
    score.enabled_count += plan->baseline_scores[node_index].enabled_count;
    score.priority += plan->baseline_scores[node_index].priority;
  }
  for (uint8_t i = 0; i < candidate->node_count; ++i) {
    const uint32_t node_index = candidate->node_indices[i];
    if (node_index != candidate->top_node_index) {
      score.required_count -= plan->best_scores[node_index].required_count;
      score.enabled_count -= plan->best_scores[node_index].enabled_count;
      score.priority -= plan->best_scores[node_index].priority;
    }
  }
  return score;
}

static void loom_low_lower_source_graph_plan_resolve_alternatives(
    loom_low_lower_source_graph_plan_t* plan,
    loom_low_lower_source_graph_alternative_query_callback_t query) {
  memset(plan->top_candidates, 0,
         plan->node_capacity * sizeof(*plan->top_candidates));
  memset(plan->singleton_nodes, 0,
         plan->node_capacity * sizeof(*plan->singleton_nodes));
  for (loom_low_lower_source_graph_candidate_t* candidate =
           plan->first_candidate;
       candidate != NULL; candidate = candidate->next) {
    candidate->phase.selection.alternative = NULL;
    loom_low_lower_source_graph_alternative_state_t selected_state =
        LOOM_LOW_LOWER_SOURCE_GRAPH_ALTERNATIVE_DISABLED;
    const loom_low_lower_source_graph_alternative_t* enabled = NULL;
    const loom_low_lower_source_graph_alternative_t* fallback = NULL;
    for (const loom_low_lower_source_graph_alternative_t* alternative =
             candidate->first_alternative;
         alternative != NULL; alternative = alternative->next) {
      const loom_low_lower_source_graph_alternative_state_t state =
          query.fn != NULL ? query.fn(query.user_data, alternative)
                           : LOOM_LOW_LOWER_SOURCE_GRAPH_ALTERNATIVE_ENABLED;
      if (state == LOOM_LOW_LOWER_SOURCE_GRAPH_ALTERNATIVE_FIXED) {
        IREE_ASSERT_EQ(candidate->node_count, 1,
                       "only singleton source graph alternatives can be fixed");
        candidate->phase.selection.alternative = alternative;
        selected_state = state;
        break;
      }
      if (state == LOOM_LOW_LOWER_SOURCE_GRAPH_ALTERNATIVE_ENABLED &&
          enabled == NULL) {
        enabled = alternative;
      }
      if (state == LOOM_LOW_LOWER_SOURCE_GRAPH_ALTERNATIVE_FALLBACK &&
          fallback == NULL) {
        fallback = alternative;
      }
    }
    if (candidate->phase.selection.alternative == NULL) {
      candidate->phase.selection.alternative =
          enabled != NULL ? enabled : fallback;
      selected_state =
          enabled != NULL    ? LOOM_LOW_LOWER_SOURCE_GRAPH_ALTERNATIVE_ENABLED
          : fallback != NULL ? LOOM_LOW_LOWER_SOURCE_GRAPH_ALTERNATIVE_FALLBACK
                             : LOOM_LOW_LOWER_SOURCE_GRAPH_ALTERNATIVE_DISABLED;
    }
    if (candidate->phase.selection.alternative == NULL) {
      continue;
    }
    candidate->selected_enabled =
        selected_state != LOOM_LOW_LOWER_SOURCE_GRAPH_ALTERNATIVE_FALLBACK;
    if (candidate->node_count == 1) {
      plan->singleton_nodes[candidate->node_indices[0]] =
          selected_state == LOOM_LOW_LOWER_SOURCE_GRAPH_ALTERNATIVE_FIXED ? 2
                                                                          : 1;
    }
  }
  for (loom_low_lower_source_graph_candidate_t* candidate =
           plan->first_candidate;
       candidate != NULL; candidate = candidate->next) {
    if (candidate->phase.selection.alternative == NULL) {
      continue;
    }
    if (candidate->node_count > 1) {
      for (uint8_t i = 0; i < candidate->node_count; ++i) {
        if (plan->singleton_nodes[candidate->node_indices[i]] == 2) {
          candidate->phase.selection.alternative = NULL;
          break;
        }
      }
    }
    if (candidate->phase.selection.alternative != NULL) {
      candidate->phase.selection.next =
          plan->top_candidates[candidate->top_node_index];
      plan->top_candidates[candidate->top_node_index] = candidate;
    }
  }
}

iree_status_t loom_low_lower_source_graph_plan_resolve(
    loom_low_lower_source_graph_plan_t* plan,
    loom_low_lower_source_graph_alternative_query_callback_t query,
    bool* out_complete) {
  *out_complete = true;
  IREE_RETURN_IF_ERROR(loom_low_lower_source_graph_plan_allocate_storage(plan));
  if (plan->node_capacity == 0) {
    return iree_ok_status();
  }

  loom_low_lower_source_graph_plan_resolve_alternatives(plan, query);
  const iree_host_size_t node_capacity = plan->node_capacity;
  memset(plan->baseline_scores, 0,
         node_capacity * sizeof(*plan->baseline_scores));
  memset(plan->best_scores, 0, node_capacity * sizeof(*plan->best_scores));
  for (iree_host_size_t i = 0; i < plan->traversal_count; ++i) {
    const uint32_t node_index = plan->traversal_queue[i];
    bool has_candidate = false;
    loom_low_lower_source_graph_score_t best_score = {0};
    loom_low_lower_source_graph_candidate_t* best_candidate = NULL;
    for (loom_low_lower_source_graph_candidate_t* candidate =
             plan->top_candidates[node_index];
         candidate != NULL; candidate = candidate->phase.selection.next) {
      const loom_low_lower_source_graph_score_t candidate_score =
          loom_low_lower_source_graph_candidate_score(plan, candidate);
      if (!has_candidate || loom_low_lower_source_graph_score_greater_equal(
                                candidate_score, best_score)) {
        has_candidate = true;
        best_score = candidate_score;
        best_candidate = candidate;
      }
    }
    const loom_low_lower_source_graph_score_t baseline_score =
        plan->baseline_scores[node_index];
    if (!has_candidate || !loom_low_lower_source_graph_score_greater_equal(
                              best_score, baseline_score)) {
      best_score = baseline_score;
      best_candidate = NULL;
    }
    plan->best_scores[node_index] = best_score;
    plan->top_candidates[node_index] = best_candidate;

    const uint32_t tree_parent = plan->tree_parents[node_index];
    if (tree_parent != LOOM_LOW_LOWER_SOURCE_GRAPH_NODE_INVALID) {
      plan->baseline_scores[tree_parent].required_count +=
          best_score.required_count;
      plan->baseline_scores[tree_parent].enabled_count +=
          best_score.enabled_count;
      plan->baseline_scores[tree_parent].priority += best_score.priority;
    }
  }

  // Walk roots to leaves. A selected ancestor candidate marks every subtree
  // node it owns before that node's local decision is considered.
  memset(plan->singleton_nodes, 0,
         node_capacity * sizeof(*plan->singleton_nodes));
  iree_host_size_t selected_node_count = 0;
  for (iree_host_size_t i = plan->traversal_count; i > 0; --i) {
    const uint32_t node_index = plan->traversal_queue[i - 1];
    if (plan->singleton_nodes[node_index]) {
      continue;
    }
    const loom_low_lower_source_graph_candidate_t* candidate =
        plan->top_candidates[node_index];
    if (candidate == NULL) {
      continue;
    }
    for (uint8_t node_ordinal = 0; node_ordinal < candidate->node_count;
         ++node_ordinal) {
      const uint32_t selected_node = candidate->node_indices[node_ordinal];
      IREE_ASSERT_FALSE(plan->singleton_nodes[selected_node]);
      plan->top_candidates[selected_node] =
          (loom_low_lower_source_graph_candidate_t*)candidate;
      plan->singleton_nodes[selected_node] = 1;
      ++selected_node_count;
    }
  }

  *out_complete = selected_node_count == plan->traversal_count;
  return iree_ok_status();
}

const loom_low_lower_source_graph_alternative_t*
loom_low_lower_source_graph_plan_selected_alternative(
    const loom_low_lower_source_graph_plan_t* plan, uint32_t node_index) {
  if (node_index >= plan->node_capacity || !plan->singleton_nodes[node_index]) {
    return NULL;
  }
  return plan->top_candidates[node_index]->phase.selection.alternative;
}

void* loom_low_lower_source_graph_alternative_user_data(
    const loom_low_lower_source_graph_alternative_t* alternative) {
  return alternative->user_data;
}

const loom_low_lower_source_graph_candidate_t*
loom_low_lower_source_graph_alternative_candidate(
    const loom_low_lower_source_graph_alternative_t* alternative) {
  return alternative->candidate;
}

void* loom_low_lower_source_graph_candidate_user_data(
    const loom_low_lower_source_graph_candidate_t* candidate) {
  return candidate->user_data;
}

void loom_low_lower_source_graph_alternative_set_candidate_user_data(
    const loom_low_lower_source_graph_alternative_t* alternative,
    void* user_data) {
  loom_low_lower_source_graph_candidate_t* candidate = alternative->candidate;
  IREE_ASSERT(candidate->user_data == NULL);
  candidate->user_data = user_data;
}

uint8_t loom_low_lower_source_graph_candidate_node_count(
    const loom_low_lower_source_graph_candidate_t* candidate) {
  return candidate->node_count;
}

uint32_t loom_low_lower_source_graph_candidate_node(
    const loom_low_lower_source_graph_candidate_t* candidate,
    uint8_t node_ordinal) {
  IREE_ASSERT_LT(node_ordinal, candidate->node_count);
  return candidate->node_indices[node_ordinal];
}
