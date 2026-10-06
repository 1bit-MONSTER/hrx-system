// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Component transport retained across a concat's physical capture boundary.

#ifndef LOOM_CODEGEN_LOW_PLACEMENT_CAPTURE_H_
#define LOOM_CODEGEN_LOW_PLACEMENT_CAPTURE_H_

#include "iree/base/api.h"
#include "iree/base/internal/arena.h"
#include "loom/analysis/liveness.h"
#include "loom/ir/ir.h"

#ifdef __cplusplus
extern "C" {
#endif

// Exceptional edge source for one concat part whose original storage changes
// while the aggregate is observable. Repeated parts can share one saved range.
typedef struct loom_low_placement_capture_t {
  // Concat part in the placement relation table.
  uint32_t relation_index;
  // Saved range inside that relation's aggregate result assignment.
  uint32_t unit_offset;
} loom_low_placement_capture_t;

// Physical component read by an aggregate's decomposed edge transport.
typedef struct loom_low_placement_concat_source_t {
  // Assignment holding the component's captured bits.
  loom_value_ordinal_t value_ordinal;
  // First unit of the component in that assignment.
  uint32_t unit_offset;
} loom_low_placement_concat_source_t;

struct loom_low_placement_table_t;

// Plans captures from retained required storage identities and semantic live
// segments before allocation shortens decomposed aggregate lifetimes. Write
// indexing and deduplication use scratch lifetime; only exceptional capture
// rows and relation flags survive in the placement table.
iree_status_t loom_low_placement_captures_build(
    const loom_liveness_analysis_t* liveness,
    struct loom_low_placement_table_t* placement, iree_arena_allocator_t* arena,
    iree_arena_allocator_t* scratch_arena);

// Returns the retained physical source for a concat part's edge reads.
loom_low_placement_concat_source_t loom_low_placement_concat_source(
    const struct loom_low_placement_table_t* placement,
    uint32_t relation_index);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_CODEGEN_LOW_PLACEMENT_CAPTURE_H_
