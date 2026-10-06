// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Arena-backed control-flow graph extraction for Loom regions.
//
// The graph is a dense view over a single region's ordered block table.
// Null targets and targets outside the region set graph->malformed and are
// omitted from adjacency. Misplaced successor-bearing ops and invalid selector
// metadata also mark the graph malformed, retaining their in-region edges.
// Analyses can answer conservatively while verification owns diagnostics.
// Entry reachability, DFS tree intervals, reverse postorder, component order,
// and reachable roots are retained by the same walk for downstream analyses.

#ifndef LOOM_UTIL_CFG_GRAPH_H_
#define LOOM_UTIL_CFG_GRAPH_H_

#include "iree/base/api.h"
#include "iree/base/internal/arena.h"
#include "loom/ir/ir.h"

#ifdef __cplusplus
extern "C" {
#endif

// A read-only span of dense block indices.
typedef struct loom_cfg_block_index_span_t {
  // Dense block indices in region block order.
  const uint16_t* values;
  // Number of entries in values.
  iree_host_size_t count;
} loom_cfg_block_index_span_t;

typedef uint32_t loom_cfg_edge_index_t;
#define LOOM_CFG_EDGE_INDEX_INVALID ((loom_cfg_edge_index_t)UINT32_MAX)

// A read-only span of dense CFG edge indices.
typedef struct loom_cfg_edge_index_span_t {
  // Dense edge indices in graph edge order.
  const loom_cfg_edge_index_t* values;
  // Number of entries in values.
  iree_host_size_t count;
} loom_cfg_edge_index_span_t;

// Per-edge CFG metadata. Edges are stable for the lifetime of the graph and
// record the originating terminator plus the concrete successor ordinal so
// analyses can ask edge-local questions without re-walking blocks.
typedef struct loom_cfg_edge_info_t {
  // Terminator op that owns this successor edge.
  const loom_op_t* terminator;
  // Dense source block index in graph block order.
  uint16_t source_block_index;
  // Dense target block index in graph block order.
  uint16_t target_block_index;
  // Successor ordinal on terminator.
  uint16_t successor_index;
  // Selector value for successor alternatives, or LOOM_VALUE_ID_INVALID when
  // the terminator has no declared selector.
  loom_value_id_t selector_value_id;
} loom_cfg_edge_info_t;

// Per-block adjacency metadata inside a CFG graph.
typedef struct loom_cfg_block_info_t {
  // Region block represented by this dense graph node.
  const loom_block_t* block;
  // Offset into loom_cfg_graph_t::successor_indices.
  iree_host_size_t successor_start;
  // Number of outgoing successor indices for this block.
  iree_host_size_t successor_count;
  // Offset into loom_cfg_graph_t::successor_edge_indices.
  iree_host_size_t successor_edge_start;
  // Offset into loom_cfg_graph_t::predecessor_indices.
  iree_host_size_t predecessor_start;
  // Number of incoming predecessor indices for this block.
  iree_host_size_t predecessor_count;
  // Offset into loom_cfg_graph_t::predecessor_edge_indices.
  iree_host_size_t predecessor_edge_start;
  // True when the block is reachable from the region entry block.
  bool reachable;
  // True when an entry-reachable block can reach a block without successors.
  // Retained during component construction; false for unreachable blocks.
  bool can_reach_exit;
  // Entry-rooted DFS preorder, or UINT16_MAX for an unreachable block.
  uint16_t preorder;
  // Dense DFS-tree parent index, or UINT16_MAX for the entry/unreachable
  // blocks.
  uint16_t parent;
  // Exclusive preorder end of a reachable block's DFS subtree. Together with
  // preorder this proves tree-path reachability without walking graph edges.
  uint16_t preorder_end;
  // Strongly connected component in successor-before-predecessor order, or
  // UINT16_MAX when unreachable. Edges between components lead to smaller
  // ordinals; distinct blocks in one component reach each other.
  uint16_t component;
  // True when this block can reach itself through one or more CFG edges.
  bool component_is_cyclic;
  // True when a reachable DFS descendant (including this block) has an edge
  // to this block. Every other reachable block finishes before all of its
  // reachable predecessors in DFS postorder.
  bool is_dfs_backedge_target;
  // Earliest DFS node reachable from this block, or UINT16_MAX if unreachable.
  // Every node in that root's DFS subtree is also reachable, including paths
  // through reconvergence.
  uint16_t reachability_root;
} loom_cfg_block_info_t;

// Dense CFG adjacency for one region.
typedef struct loom_cfg_graph_t {
  // Module containing region.
  const loom_module_t* module;
  // Region whose block table defines the dense graph node order.
  const loom_region_t* region;
  // Per-block adjacency metadata with block_count entries.
  loom_cfg_block_info_t* blocks;
  // Dense edge metadata with edge_count entries.
  loom_cfg_edge_info_t* edges;
  // Dense target block indices for all outgoing edges.
  uint16_t* successor_indices;
  // Dense edge indices for all outgoing edges.
  loom_cfg_edge_index_t* successor_edge_indices;
  // Dense source block indices for all incoming edges.
  uint16_t* predecessor_indices;
  // Dense edge indices for all incoming edges.
  loom_cfg_edge_index_t* predecessor_edge_indices;
  // Number of dense block nodes.
  iree_host_size_t block_count;
  // Number of valid in-region successor edges.
  iree_host_size_t edge_count;
  // Reachable blocks in reverse DFS postorder, starting at the entry block.
  // Successors are visited in declared order. Every strict dominator precedes
  // its dominated blocks; unreachable blocks have no entry in this span.
  loom_cfg_block_index_span_t reverse_postorder;
  // Number of edges whose target does not follow their source in region block
  // order. This is a cheap rejection fact for loop analyses; a backward edge
  // is not necessarily a semantic CFG backedge.
  iree_host_size_t backward_edge_count;
  // True when the entry-reachable graph contains a cycle, including self-edges.
  // Independent of region block order; unreachable cycles do not contribute.
  bool has_cycles;
  // True when malformed successor structure was seen while building the graph.
  bool malformed;
} loom_cfg_graph_t;

// Returns the argument payload that |terminator| forwards to |successor| when
// it can be represented generically. Today this is intentionally limited to
// single-successor terminators whose operands map 1:1 onto successor block
// arguments.
bool loom_cfg_terminator_payload_for_successor(const loom_op_t* terminator,
                                               const loom_block_t* successor,
                                               const loom_value_id_t** out_args,
                                               uint16_t* out_arg_count);

// Builds a dense CFG graph for |region| and stores all result memory in
// |arena|. Passing NULL arguments is an API error. Malformed IR is represented
// by graph->malformed rather than a failing status so callers can stay
// conservative and let verification produce structured diagnostics.
iree_status_t loom_cfg_graph_build(const loom_module_t* module,
                                   const loom_region_t* region,
                                   iree_arena_allocator_t* arena,
                                   loom_cfg_graph_t* out_graph);

// Returns the dense index of |block| in |graph|, or IREE_HOST_SIZE_MAX when the
// block is not owned by the graph's region.
iree_host_size_t loom_cfg_graph_block_index(const loom_cfg_graph_t* graph,
                                            const loom_block_t* block);

// Returns outgoing successor block indices for |block_index|.
loom_cfg_block_index_span_t loom_cfg_graph_successors(
    const loom_cfg_graph_t* graph, uint16_t block_index);

// Returns outgoing successor edge indices for |block_index|.
loom_cfg_edge_index_span_t loom_cfg_graph_successor_edges(
    const loom_cfg_graph_t* graph, uint16_t block_index);

// Returns incoming predecessor block indices for |block_index|.
loom_cfg_block_index_span_t loom_cfg_graph_predecessors(
    const loom_cfg_graph_t* graph, uint16_t block_index);

// Returns incoming predecessor edge indices for |block_index|.
loom_cfg_edge_index_span_t loom_cfg_graph_predecessor_edges(
    const loom_cfg_graph_t* graph, uint16_t block_index);

// Returns edge metadata for |edge_index|, or NULL when out of range.
const loom_cfg_edge_info_t* loom_cfg_graph_edge(
    const loom_cfg_graph_t* graph, loom_cfg_edge_index_t edge_index);

// Returns true when |block_index| is reachable from the region entry block.
bool loom_cfg_graph_block_is_reachable(const loom_cfg_graph_t* graph,
                                       uint16_t block_index);

// Refreshes the cached selector identity after an operand edit that preserves
// CFG topology. Returns the current selector, or INVALID for an unconditional
// terminator. All alternatives retain the same current identity.
loom_value_id_t loom_cfg_graph_refresh_selector(const loom_cfg_graph_t* graph,
                                                uint16_t block_index);

#ifdef __cplusplus
}
#endif

#endif  // LOOM_UTIL_CFG_GRAPH_H_
