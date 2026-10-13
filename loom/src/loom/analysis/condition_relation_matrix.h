// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Sparse condition-relation matrices for propagation and retained queries.

#ifndef LOOM_ANALYSIS_CONDITION_RELATION_MATRIX_H_
#define LOOM_ANALYSIS_CONDITION_RELATION_MATRIX_H_

#include "iree/base/api.h"
#include "iree/base/internal/arena.h"
#include "loom/analysis/condition_relation_set.h"
#include "loom/analysis/integer_relation.h"

#ifdef __cplusplus
extern "C" {
#endif

// One mutable sparse relation row.
typedef struct loom_condition_relation_matrix_row_t {
  // Canonical left-value ordinal in the same domain as the row's set roots.
  uint32_t left;

  // Right-value sets excluded for less, equal, and greater outcomes.
  loom_condition_relation_set_id_t excluded[LOOM_INTEGER_ORDER_OUTCOME_COUNT];
} loom_condition_relation_matrix_row_t;

// Sorted mutable rows used by the finite propagation solver.
typedef struct loom_condition_relation_matrix_t {
  // Rows sorted by ascending left-value ordinal.
  loom_condition_relation_matrix_row_t* rows;

  // Number of rows, including stable empty propagation tombstones.
  uint32_t row_count;
} loom_condition_relation_matrix_t;

// Returns true when |row| excludes no right values.
static inline bool loom_condition_relation_matrix_row_is_empty(
    const loom_condition_relation_matrix_row_t* row) {
  return row->excluded[0] == LOOM_CONDITION_RELATION_SET_EMPTY &&
         row->excluded[1] == LOOM_CONDITION_RELATION_SET_EMPTY &&
         row->excluded[2] == LOOM_CONDITION_RELATION_SET_EMPTY;
}

// Temporary accumulator for unsorted and duplicate matrix rows.
typedef struct loom_condition_relation_matrix_builder_t {
  // Arena owning temporary candidate rows.
  iree_arena_allocator_t* arena;

  // Unsorted candidate rows.
  loom_condition_relation_matrix_row_t* rows;

  // Number of initialized candidate rows.
  iree_host_size_t row_count;

  // Allocated candidate-row capacity.
  iree_host_size_t row_capacity;
} loom_condition_relation_matrix_builder_t;

// Initializes an empty builder without allocating.
void loom_condition_relation_matrix_builder_initialize(
    iree_arena_allocator_t* arena,
    loom_condition_relation_matrix_builder_t* out_builder);

// Resets a builder while retaining its arena-backed high-water storage.
void loom_condition_relation_matrix_builder_reset(
    loom_condition_relation_matrix_builder_t* builder);

// Appends one excluded right-value set. Empty sets require no row and are
// ignored.
iree_status_t loom_condition_relation_matrix_builder_add(
    loom_condition_relation_matrix_builder_t* builder,
    loom_integer_order_outcome_t outcome, uint32_t left,
    loom_condition_relation_set_id_t excluded);

// Appends all nonempty rows from |matrix|.
iree_status_t loom_condition_relation_matrix_builder_add_matrix(
    loom_condition_relation_matrix_builder_t* builder,
    const loom_condition_relation_matrix_t* matrix);

// Sorts and folds candidate rows, unioning duplicate left/outcome sets, and
// copies exact result storage into |arena|. The builder remains scratch-owned
// and its candidate order is consumed by the operation.
iree_status_t loom_condition_relation_matrix_builder_build(
    loom_condition_relation_matrix_builder_t* builder,
    loom_condition_relation_set_builder_t* set_builder,
    iree_arena_allocator_t* arena,
    loom_condition_relation_matrix_t* out_matrix);

// Copies |source| into exact arena-backed mutable storage.
iree_status_t loom_condition_relation_matrix_clone(
    const loom_condition_relation_matrix_t* source,
    iree_arena_allocator_t* arena,
    loom_condition_relation_matrix_t* out_matrix);

// Returns the mutable row for |left|, or NULL when absent.
loom_condition_relation_matrix_row_t* loom_condition_relation_matrix_find(
    loom_condition_relation_matrix_t* matrix, uint32_t left);

// Returns the row for |left|, or NULL when absent.
const loom_condition_relation_matrix_row_t*
loom_condition_relation_matrix_find_const(
    const loom_condition_relation_matrix_t* matrix, uint32_t left);

// Replaces one excluded set in an existing row and returns whether it changed.
bool loom_condition_relation_matrix_set(
    loom_condition_relation_matrix_t* matrix,
    loom_integer_order_outcome_t outcome, uint32_t left,
    loom_condition_relation_set_id_t excluded);

// Intersects every destination set with its matching set in |other|. Missing
// rows and outcomes contribute the empty set. Destination row positions remain
// stable so propagation events may continue to identify them by index.
iree_status_t loom_condition_relation_matrix_intersect_into(
    loom_condition_relation_set_builder_t* set_builder,
    loom_condition_relation_matrix_t* destination,
    const loom_condition_relation_matrix_t* other);

// Encoding used by immutable matrix entries.
typedef uint8_t loom_condition_relation_matrix_view_encoding_t;
enum loom_condition_relation_matrix_view_encoding_e {
  LOOM_CONDITION_RELATION_MATRIX_VIEW_SPARSE = 0,
  LOOM_CONDITION_RELATION_MATRIX_VIEW_RANGES = 1,
  LOOM_CONDITION_RELATION_MATRIX_VIEW_PAGES = 2,
};

typedef struct loom_condition_relation_matrix_page_t
    loom_condition_relation_matrix_page_t;

// One consecutive run of left values sharing identical excluded sets.
typedef struct loom_condition_relation_matrix_range_t {
  // First left-value ordinal in the same domain as the range's set roots.
  uint32_t first_left;

  // Number of consecutive left-value ordinals covered by this range.
  uint32_t left_count;

  // Right-value sets excluded for every left value in the range.
  loom_condition_relation_set_id_t excluded[LOOM_INTEGER_ORDER_OUTCOME_COUNT];
} loom_condition_relation_matrix_range_t;

// Encoding-specific immutable entry storage.
typedef union loom_condition_relation_matrix_view_entries_t {
  // Sparse rows sorted by explicit left-value ordinal.
  const loom_condition_relation_matrix_row_t* rows;

  // Ranges sorted by first left-value ordinal.
  const loom_condition_relation_matrix_range_t* ranges;

  // Shared pages sorted by their first left-value ordinal.
  const loom_condition_relation_matrix_page_t* const* pages;
} loom_condition_relation_matrix_view_entries_t;

// Immutable matrix view retained after propagation scratch is released.
typedef struct loom_condition_relation_matrix_view_t {
  // Encoding-specific immutable entries.
  loom_condition_relation_matrix_view_entries_t entries;

  // Number of immutable entries.
  uint32_t entry_count;

  // Encoding interpreting entries.
  loom_condition_relation_matrix_view_encoding_t encoding;
} loom_condition_relation_matrix_view_t;

// One shared interval of an immutable matrix view. Page contents use sparse or
// range encoding and never recursively contain pages.
struct loom_condition_relation_matrix_page_t {
  // First possible left-value ordinal in the page.
  uint32_t first_left;

  // Last possible left-value ordinal in the page, inclusive.
  uint32_t last_left;

  // Immutable rows retained for the page interval.
  loom_condition_relation_matrix_view_t contents;
};

// Publishes the smaller exact encoding of nonempty sparse rows or consecutive
// equal ranges into |arena|. Set roots must already be rewritten into their
// immutable index ID space.
iree_status_t loom_condition_relation_matrix_view_publish(
    const loom_condition_relation_matrix_t* source,
    iree_arena_allocator_t* arena,
    loom_condition_relation_matrix_view_t* out_view);

// Returns the three excluded set roots for |left| after one binary search, or
// NULL when no retained row covers |left|. The returned array is view-owned.
const loom_condition_relation_set_id_t*
loom_condition_relation_matrix_view_find(
    const loom_condition_relation_matrix_view_t* view, uint32_t left);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_ANALYSIS_CONDITION_RELATION_MATRIX_H_
