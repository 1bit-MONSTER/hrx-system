// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Target-independent Low loop execution facts.

#ifndef LOOM_CODEGEN_LOW_LOOP_EXECUTION_H_
#define LOOM_CODEGEN_LOW_LOOP_EXECUTION_H_

#include "iree/base/api.h"
#include "iree/base/internal/arena.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct loom_block_t loom_block_t;
typedef struct loom_op_t loom_op_t;
typedef struct loom_region_t loom_region_t;

// Precision of a retained Low loop execution count. An absent binding means
// that no finite count is known.
enum loom_low_loop_execution_count_kind_e {
  // The count is an inclusive upper bound.
  LOOM_LOW_LOOP_EXECUTION_COUNT_BOUNDED = 0,
  // Every represented entry executes the body exactly this many times.
  LOOM_LOW_LOOP_EXECUTION_COUNT_EXACT,
};
typedef uint8_t loom_low_loop_execution_count_kind_t;

// Producer-proven body execution count for one natural loop.
typedef struct loom_low_loop_execution_facts_t {
  // Inclusive maximum for BOUNDED, or exact count for EXACT.
  uint64_t trip_count;
  // Precision of |trip_count|. Retained records are BOUNDED or EXACT.
  loom_low_loop_execution_count_kind_t count_kind;
} loom_low_loop_execution_facts_t;

static_assert(sizeof(loom_low_loop_execution_facts_t) == 16,
              "Low loop execution facts must remain compact");

// Compilation-local facts bound to the exact Low control operations produced
// for source loop headers. Operation identity follows block fusion and motion;
// erased controls leave inert records that indexed consumers ignore.
typedef struct loom_low_loop_execution_map_t loom_low_loop_execution_map_t;

// Allocates an initially empty sparse map in |arena|.
iree_status_t loom_low_loop_execution_map_allocate(
    iree_arena_allocator_t* arena, loom_low_loop_execution_map_t** out_map);

// Binds producer-proven execution facts to one live Low loop control op.
iree_status_t loom_low_loop_execution_map_insert(
    loom_low_loop_execution_map_t* map, const loom_op_t* control_op,
    loom_low_loop_execution_facts_t facts);

// Transfers bindings for operations moved within the same module.
iree_status_t loom_low_loop_execution_map_transfer(
    const loom_low_loop_execution_map_t* source,
    loom_low_loop_execution_map_t* target);

// Scratch projection of sparse facts onto the current block order of one
// function body. Construction is linear in blocks plus retained records.
typedef struct loom_low_loop_execution_index_t {
  // Facts indexed by current block index, or NULL for an unproven block.
  const loom_low_loop_execution_facts_t* const* facts_by_block;
  // Number of entries in |facts_by_block|.
  iree_host_size_t block_count;
} loom_low_loop_execution_index_t;

// Builds a direct block index over live controls currently in |body|.
iree_status_t loom_low_loop_execution_index_build(
    const loom_low_loop_execution_map_t* map, const loom_region_t* body,
    iree_arena_allocator_t* arena, loom_low_loop_execution_index_t* out_index);

// Returns facts bound to |header|'s current terminator, or NULL.
const loom_low_loop_execution_facts_t* loom_low_loop_execution_index_lookup(
    const loom_low_loop_execution_index_t* index, const loom_block_t* header);

// One operation-clone invocation translating bound control identities. The
// source map is immutable; target bindings are appended to the target map.
typedef struct loom_low_loop_execution_clone_t loom_low_loop_execution_clone_t;

iree_status_t loom_low_loop_execution_clone_allocate(
    const loom_low_loop_execution_map_t* source,
    loom_low_loop_execution_map_t* target,
    iree_arena_allocator_t* scratch_arena,
    loom_low_loop_execution_clone_t** out_clone);

// Matches the generic operation-clone observer signature.
iree_status_t loom_low_loop_execution_clone_op(void* clone,
                                               const loom_op_t* source_op,
                                               loom_op_t* target_op);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_CODEGEN_LOW_LOOP_EXECUTION_H_
