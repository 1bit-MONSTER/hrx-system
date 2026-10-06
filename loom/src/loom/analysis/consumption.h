// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Queries for linear value consumption.
//
// Tied and moved results consume their source operands: after the consuming op
// executes along one dynamic path, later operations on that same path must
// observe the result, not the consumed value. CFG regions make that a
// path-sensitive question because reentering a value's defining block creates
// a new dynamic instance, whether defined by an argument or an operation.

#ifndef LOOM_ANALYSIS_CONSUMPTION_H_
#define LOOM_ANALYSIS_CONSUMPTION_H_

#include "iree/base/api.h"
#include "iree/base/internal/arena.h"
#include "loom/analysis/liveness.h"
#include "loom/ir/ir.h"
#include "loom/util/cfg_graph.h"

#ifdef __cplusplus
extern "C" {
#endif

// Operand occurrence selected by a consumption query.
typedef struct loom_consumption_use_t {
  // Operation containing the observing operand.
  const loom_op_t* op;
  // Operand index on |op| that observes the value.
  uint16_t operand_index;
} loom_consumption_use_t;

// Finds a tied- or moved-result operand that consumes |value|, if present.
// Mutually exclusive control-flow paths may each contain a consuming
// occurrence. |out_use| receives the first retained occurrence and may be NULL
// when only membership is needed.
bool loom_consumption_find_consuming_use(const loom_module_t* module,
                                         const loom_value_t* value,
                                         loom_consumption_use_t* out_use);

// Reusable per-region query state for consumed-value checks.
typedef struct loom_consumption_region_query_t {
  // Module containing the queried region.
  const loom_module_t* module;
  // Region whose dynamic paths are queried.
  const loom_region_t* region;
  // Arena used for owned CFG extraction and reusable search scratch.
  iree_arena_allocator_t* arena;
  // CFG graph for the region, built lazily or copied from shared analysis.
  loom_cfg_graph_t cfg_graph;
  // True once cfg_graph has been initialized.
  bool cfg_graph_ready;
  // Optional borrowed liveness proving whether a value survives block exit.
  const loom_liveness_analysis_t* liveness;
  // Acquired domain mapping queried values to the borrowed liveness ordinals.
  const loom_local_value_domain_t* value_domain;
  // Reusable visited bitset for CFG searches.
  uint64_t* visited_bits;
  // Allocated word capacity of visited_bits.
  iree_host_size_t visited_word_capacity;
  // Reusable bitset of blocks reachable before |value_id| is recreated.
  uint64_t* reachable_bits;
  // Allocated word capacity of reachable_bits.
  iree_host_size_t reachable_word_capacity;
  // Visited dense CFG block indices. Retained entries identify
  // exactly which visited/reachable bits must be cleared before the next query.
  uint16_t* visited_blocks;
  // Allocated element capacity of visited_blocks.
  iree_host_size_t visited_block_capacity;
  // Number of entries in visited_blocks for the preceding or current query.
  iree_host_size_t visited_block_count;
  // Pending blocks in a max-heap ordered by CFG component ordinal. A query
  // only advances until no pending component can reach its requested block.
  uint16_t* block_heap;
  // Allocated element capacity of block_heap.
  iree_host_size_t block_heap_capacity;
  // Number of pending entries in block_heap.
  iree_host_size_t pending_block_count;
} loom_consumption_region_query_t;

// Prepared dynamic-path query for uses after one block-local boundary.
//
// The query borrows scratch from |region_query| and remains valid until the
// next use-after query is prepared from that region query.
typedef struct loom_consumption_use_after_query_t {
  // Reusable region query owning CFG and reachability scratch.
  loom_consumption_region_query_t* region_query;
  // Block containing the boundary after which observations are queried.
  const loom_block_t* origin_block;
  // Value whose dynamic instance is being followed.
  loom_value_id_t value_id;
  // True once a membership query initializes the resumable CFG frontier.
  bool search_initialized;
  // First observable sparse position key in the initial visit to origin_block.
  // Uses the IR's full-width order-maintenance labels, not a dense op index.
  uint64_t first_observation_ordinal;
  // Value's defining block. Reentering it ends the old dynamic instance.
  const loom_block_t* recreation_block;
  // Number of words available in region_query->reachable_bits.
  iree_host_size_t reachable_word_count;
} loom_consumption_use_after_query_t;

// Initializes reusable consumption query state for |region|. CFG extraction is
// lazy: regions without consumed values do not pay graph construction.
void loom_consumption_region_query_initialize(
    const loom_module_t* module, const loom_region_t* region,
    iree_arena_allocator_t* arena, loom_consumption_region_query_t* out_query);

// Initializes reusable consumption query state with a prebuilt CFG graph.
//
// The query copies the graph view and borrows its arena-owned arrays. The graph
// must describe |region| and remain immutable while the query is used.
// Optional |liveness| and |value_domain| are supplied together and describe the
// same region. The domain remains acquired and all facts remain immutable for
// the query lifetime. Queries without those facts pass NULL for both.
void loom_consumption_region_query_initialize_with_cfg_graph(
    const loom_module_t* module, const loom_region_t* region,
    const loom_cfg_graph_t* cfg_graph, const loom_liveness_analysis_t* liveness,
    const loom_local_value_domain_t* value_domain,
    iree_arena_allocator_t* arena, loom_consumption_region_query_t* out_query);

enum loom_consumption_query_flag_bits_e {
  // Observations include producer-proven indirect reads not present in the
  // value's semantic SSA use list or live-out segments.
  LOOM_CONSUMPTION_QUERY_FLAG_INDIRECT_OBSERVATIONS = 1u << 0,
};
typedef uint8_t loom_consumption_query_flags_t;

// Prepares a reusable path query for observations of |value_id| starting at
// |first_observation_ordinal| in |origin_block|. Zero includes the block entry;
// a consuming operation's ordinal plus one queries only later observations.
// Membership queries consume retained CFG path proofs first; unresolved
// queries advance one shared frontier only through the requested component.
// Each block is expanded at most once before the next preparation, without
// scanning IR operations.
iree_status_t loom_consumption_use_after_query_prepare(
    loom_consumption_region_query_t* region_query,
    const loom_block_t* origin_block, uint64_t first_observation_ordinal,
    loom_value_id_t value_id, loom_consumption_query_flags_t flags,
    loom_consumption_use_after_query_t* out_query);

// Returns true when |use| can dynamically execute after the boundary
// represented by |query|. |use| must belong to the queried value.
bool loom_consumption_use_after_query_contains(
    loom_consumption_use_after_query_t* query, loom_use_t use);

// Tests a producer-proven observation of the queried dynamic value at |op|.
// Indirect observations require INDIRECT_OBSERVATIONS during preparation;
// their source's recreation boundary remains the queried value's definition.
bool loom_consumption_use_after_query_observes_operation(
    loom_consumption_use_after_query_t* query, const loom_op_t* op);

// Finds a use of |value_id| that can dynamically execute after |consuming_op|.
// |query| must describe |consuming_op|'s parent region. The value's use list
// supplies candidates; non-CFG regions only check later uses in the same block,
// while CFG regions reuse graph and reachability scratch across calls.
iree_status_t loom_consumption_find_use_after(
    loom_consumption_region_query_t* query, const loom_op_t* consuming_op,
    loom_value_id_t value_id, loom_consumption_use_t* out_use, bool* out_found);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_ANALYSIS_CONSUMPTION_H_
