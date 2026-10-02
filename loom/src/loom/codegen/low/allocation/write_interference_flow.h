// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Control-flow spans for retained reads at allocation write points.

#ifndef LOOM_CODEGEN_LOW_ALLOCATION_WRITE_INTERFERENCE_FLOW_H_
#define LOOM_CODEGEN_LOW_ALLOCATION_WRITE_INTERFERENCE_FLOW_H_

#include "loom/analysis/liveness.h"
#include "loom/util/cfg_graph.h"

// A maximal linear span of write points, including structured entry/exit gaps.
typedef struct loom_low_write_flow_block_t {
  // First included write point.
  uint32_t begin;
  // Exclusive end write point.
  uint32_t end;
  // First outgoing successor in the graph's edge array.
  uint32_t successor_start;
  // Number of outgoing successors.
  uint32_t successor_count;
} loom_low_write_flow_block_t;

// Root CFG refined at structured branches and backedges. Constructed once from
// liveness's accepted operation points, without requiring a schedule table.
typedef struct loom_low_write_flow_t {
  // Arena-owned maximal linear point spans.
  loom_low_write_flow_block_t* blocks;
  // Number of spans.
  uint32_t block_count;
  // Arena-owned successor block indices.
  uint32_t* successors;
  // Recycled construction buffer with at least |block_count| uninitialized
  // entries for the solver's worklist; no graph facts remain in this storage.
  uint32_t* worklist;
} loom_low_write_flow_t;

iree_status_t loom_low_write_flow_build(
    const loom_liveness_analysis_t* liveness, const loom_cfg_graph_t* cfg_graph,
    uint32_t point_count, iree_arena_allocator_t* arena,
    loom_low_write_flow_t* out_flow);

#endif  // LOOM_CODEGEN_LOW_ALLOCATION_WRITE_INTERFERENCE_FLOW_H_
