// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/analysis/condition_relation_matrix.h"

#include <string.h>

#include "loom/util/adaptive_sort.h"

static_assert(sizeof(loom_condition_relation_matrix_row_t) == 16,
              "sparse condition-relation rows must remain 16 bytes");
static_assert(sizeof(loom_condition_relation_matrix_range_t) == 20,
              "condition-relation ranges must remain 20 bytes");
static_assert(sizeof(loom_condition_relation_matrix_view_t) ==
                  (IREE_PTR_SIZE == 8 ? 16 : 12),
              "condition-relation views must remain compact");

static bool loom_condition_relation_matrix_row_less(
    const loom_condition_relation_matrix_row_t* left,
    const loom_condition_relation_matrix_row_t* right) {
  return left->left < right->left;
}

LOOM_DEFINE_ADAPTIVE_SORT(loom_condition_relation_matrix_sort_rows,
                          loom_condition_relation_matrix_row_t,
                          loom_condition_relation_matrix_row_less)

static iree_status_t loom_condition_relation_matrix_builder_reserve(
    loom_condition_relation_matrix_builder_t* builder,
    iree_host_size_t minimum_capacity) {
  if (minimum_capacity <= builder->row_capacity) {
    return iree_ok_status();
  }
  minimum_capacity = iree_max((iree_host_size_t)16, minimum_capacity);
  return iree_arena_grow_array(builder->arena, builder->row_count,
                               minimum_capacity, sizeof(*builder->rows),
                               &builder->row_capacity, (void**)&builder->rows);
}

void loom_condition_relation_matrix_builder_initialize(
    iree_arena_allocator_t* arena,
    loom_condition_relation_matrix_builder_t* out_builder) {
  *out_builder = (loom_condition_relation_matrix_builder_t){
      .arena = arena,
  };
}

void loom_condition_relation_matrix_builder_reset(
    loom_condition_relation_matrix_builder_t* builder) {
  builder->row_count = 0;
}

iree_status_t loom_condition_relation_matrix_builder_add(
    loom_condition_relation_matrix_builder_t* builder,
    loom_condition_relation_outcome_t outcome, uint32_t left,
    loom_condition_relation_set_id_t excluded) {
  IREE_ASSERT_LT(outcome, LOOM_CONDITION_RELATION_OUTCOME_COUNT);
  if (excluded == LOOM_CONDITION_RELATION_SET_EMPTY) {
    return iree_ok_status();
  }
  IREE_RETURN_IF_ERROR(loom_condition_relation_matrix_builder_reserve(
      builder, builder->row_count + 1));
  loom_condition_relation_matrix_row_t* row =
      &builder->rows[builder->row_count++];
  *row = (loom_condition_relation_matrix_row_t){
      .left = left,
  };
  row->excluded[outcome] = excluded;
  return iree_ok_status();
}

iree_status_t loom_condition_relation_matrix_builder_add_matrix(
    loom_condition_relation_matrix_builder_t* builder,
    const loom_condition_relation_matrix_t* matrix) {
  for (uint32_t i = 0; i < matrix->row_count; ++i) {
    const loom_condition_relation_matrix_row_t* row = &matrix->rows[i];
    if (loom_condition_relation_matrix_row_is_empty(row)) {
      continue;
    }
    IREE_RETURN_IF_ERROR(loom_condition_relation_matrix_builder_reserve(
        builder, builder->row_count + 1));
    builder->rows[builder->row_count++] = *row;
  }
  return iree_ok_status();
}

iree_status_t loom_condition_relation_matrix_builder_build(
    loom_condition_relation_matrix_builder_t* builder,
    loom_condition_relation_set_builder_t* set_builder,
    iree_arena_allocator_t* arena,
    loom_condition_relation_matrix_t* out_matrix) {
  *out_matrix = (loom_condition_relation_matrix_t){0};
  if (builder->row_count == 0) {
    return iree_ok_status();
  }
  loom_condition_relation_matrix_sort_rows(builder->rows, builder->row_count);

  iree_host_size_t row_count = 0;
  for (iree_host_size_t i = 0; i < builder->row_count; ++i) {
    const loom_condition_relation_matrix_row_t source = builder->rows[i];
    if (row_count == 0 || builder->rows[row_count - 1].left != source.left) {
      if (row_count == UINT32_MAX) {
        return iree_make_status(
            IREE_STATUS_RESOURCE_EXHAUSTED,
            "condition-relation matrix row capacity exceeded");
      }
      builder->rows[row_count++] = source;
      continue;
    }
    loom_condition_relation_matrix_row_t* destination =
        &builder->rows[row_count - 1];
    for (loom_condition_relation_outcome_t outcome = 0;
         outcome < LOOM_CONDITION_RELATION_OUTCOME_COUNT; ++outcome) {
      if (source.excluded[outcome] == LOOM_CONDITION_RELATION_SET_EMPTY) {
        continue;
      }
      IREE_RETURN_IF_ERROR(loom_condition_relation_set_builder_union(
          set_builder, destination->excluded[outcome], source.excluded[outcome],
          &destination->excluded[outcome]));
    }
  }
  builder->row_count = row_count;

  loom_condition_relation_matrix_row_t* rows = NULL;
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(arena, row_count,
                                                 sizeof(*rows), (void**)&rows));
  memcpy(rows, builder->rows, row_count * sizeof(*rows));
  *out_matrix = (loom_condition_relation_matrix_t){
      .rows = rows,
      .row_count = (uint32_t)row_count,
  };
  return iree_ok_status();
}

iree_status_t loom_condition_relation_matrix_clone(
    const loom_condition_relation_matrix_t* source,
    iree_arena_allocator_t* arena,
    loom_condition_relation_matrix_t* out_matrix) {
  *out_matrix = (loom_condition_relation_matrix_t){0};
  if (source->row_count == 0) {
    return iree_ok_status();
  }
  loom_condition_relation_matrix_row_t* rows = NULL;
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(arena, source->row_count,
                                                 sizeof(*rows), (void**)&rows));
  memcpy(rows, source->rows, source->row_count * sizeof(*rows));
  *out_matrix = (loom_condition_relation_matrix_t){
      .rows = rows,
      .row_count = source->row_count,
  };
  return iree_ok_status();
}

static uint32_t loom_condition_relation_matrix_find_index(
    const loom_condition_relation_matrix_t* matrix, uint32_t left) {
  uint32_t begin = 0;
  uint32_t end = matrix->row_count;
  while (begin < end) {
    const uint32_t middle = begin + (end - begin) / 2;
    if (matrix->rows[middle].left < left) {
      begin = middle + 1;
    } else {
      end = middle;
    }
  }
  return begin;
}

loom_condition_relation_matrix_row_t* loom_condition_relation_matrix_find(
    loom_condition_relation_matrix_t* matrix, uint32_t left) {
  const uint32_t index =
      loom_condition_relation_matrix_find_index(matrix, left);
  return index < matrix->row_count && matrix->rows[index].left == left
             ? &matrix->rows[index]
             : NULL;
}

const loom_condition_relation_matrix_row_t*
loom_condition_relation_matrix_find_const(
    const loom_condition_relation_matrix_t* matrix, uint32_t left) {
  const uint32_t index =
      loom_condition_relation_matrix_find_index(matrix, left);
  return index < matrix->row_count && matrix->rows[index].left == left
             ? &matrix->rows[index]
             : NULL;
}

bool loom_condition_relation_matrix_set(
    loom_condition_relation_matrix_t* matrix,
    loom_condition_relation_outcome_t outcome, uint32_t left,
    loom_condition_relation_set_id_t excluded) {
  IREE_ASSERT_LT(outcome, LOOM_CONDITION_RELATION_OUTCOME_COUNT);
  loom_condition_relation_matrix_row_t* row =
      loom_condition_relation_matrix_find(matrix, left);
  IREE_ASSERT(row);
  const loom_condition_relation_set_id_t previous = row->excluded[outcome];
  row->excluded[outcome] = excluded;
  return previous != excluded;
}

iree_status_t loom_condition_relation_matrix_intersect_into(
    loom_condition_relation_set_builder_t* set_builder,
    loom_condition_relation_matrix_t* destination,
    const loom_condition_relation_matrix_t* other) {
  uint32_t other_position = 0;
  for (uint32_t i = 0; i < destination->row_count; ++i) {
    loom_condition_relation_matrix_row_t* destination_row =
        &destination->rows[i];
    while (other_position < other->row_count &&
           other->rows[other_position].left < destination_row->left) {
      ++other_position;
    }
    const loom_condition_relation_matrix_row_t* other_row =
        other_position < other->row_count &&
                other->rows[other_position].left == destination_row->left
            ? &other->rows[other_position]
            : NULL;
    for (loom_condition_relation_outcome_t outcome = 0;
         outcome < LOOM_CONDITION_RELATION_OUTCOME_COUNT; ++outcome) {
      const loom_condition_relation_set_id_t other_excluded =
          other_row ? other_row->excluded[outcome]
                    : LOOM_CONDITION_RELATION_SET_EMPTY;
      IREE_RETURN_IF_ERROR(loom_condition_relation_set_builder_intersection(
          set_builder, destination_row->excluded[outcome], other_excluded,
          &destination_row->excluded[outcome]));
    }
  }
  return iree_ok_status();
}

static bool loom_condition_relation_matrix_rows_extend_range(
    const loom_condition_relation_matrix_row_t* previous,
    const loom_condition_relation_matrix_row_t* current) {
  return previous->left != UINT32_MAX && current->left == previous->left + 1 &&
         previous->excluded[0] == current->excluded[0] &&
         previous->excluded[1] == current->excluded[1] &&
         previous->excluded[2] == current->excluded[2];
}

static void loom_condition_relation_matrix_count_publication(
    const loom_condition_relation_matrix_t* source,
    uint32_t* out_live_row_count, uint32_t* out_range_count) {
  uint32_t live_row_count = 0;
  uint32_t range_count = 0;
  const loom_condition_relation_matrix_row_t* previous = NULL;
  for (uint32_t i = 0; i < source->row_count; ++i) {
    const loom_condition_relation_matrix_row_t* row = &source->rows[i];
    if (loom_condition_relation_matrix_row_is_empty(row)) {
      continue;
    }
    ++live_row_count;
    range_count +=
        !previous ||
        !loom_condition_relation_matrix_rows_extend_range(previous, row);
    previous = row;
  }
  *out_live_row_count = live_row_count;
  *out_range_count = range_count;
}

static iree_status_t loom_condition_relation_matrix_publish_sparse(
    const loom_condition_relation_matrix_t* source, uint32_t live_row_count,
    iree_arena_allocator_t* arena,
    loom_condition_relation_matrix_view_t* out_view) {
  loom_condition_relation_matrix_row_t* rows = NULL;
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(arena, live_row_count,
                                                 sizeof(*rows), (void**)&rows));
  uint32_t output_count = 0;
  for (uint32_t i = 0; i < source->row_count; ++i) {
    if (!loom_condition_relation_matrix_row_is_empty(&source->rows[i])) {
      rows[output_count++] = source->rows[i];
    }
  }
  *out_view = (loom_condition_relation_matrix_view_t){
      .entries.rows = rows,
      .entry_count = output_count,
      .encoding = LOOM_CONDITION_RELATION_MATRIX_VIEW_SPARSE,
  };
  return iree_ok_status();
}

static iree_status_t loom_condition_relation_matrix_publish_ranges(
    const loom_condition_relation_matrix_t* source, uint32_t range_count,
    iree_arena_allocator_t* arena,
    loom_condition_relation_matrix_view_t* out_view) {
  loom_condition_relation_matrix_range_t* ranges = NULL;
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      arena, range_count, sizeof(*ranges), (void**)&ranges));
  uint32_t output_count = 0;
  const loom_condition_relation_matrix_row_t* previous = NULL;
  for (uint32_t i = 0; i < source->row_count; ++i) {
    const loom_condition_relation_matrix_row_t* row = &source->rows[i];
    if (loom_condition_relation_matrix_row_is_empty(row)) {
      continue;
    }
    if (previous &&
        loom_condition_relation_matrix_rows_extend_range(previous, row)) {
      ++ranges[output_count - 1].left_count;
    } else {
      ranges[output_count++] = (loom_condition_relation_matrix_range_t){
          .first_left = row->left,
          .left_count = 1,
          .excluded = {row->excluded[0], row->excluded[1], row->excluded[2]},
      };
    }
    previous = row;
  }
  *out_view = (loom_condition_relation_matrix_view_t){
      .entries.ranges = ranges,
      .entry_count = output_count,
      .encoding = LOOM_CONDITION_RELATION_MATRIX_VIEW_RANGES,
  };
  return iree_ok_status();
}

#if IREE_HAVE_ATTRIBUTE(minsize)
__attribute__((minsize))
#endif
IREE_ATTRIBUTE_NOINLINE iree_status_t
loom_condition_relation_matrix_view_publish(
    const loom_condition_relation_matrix_t* source,
    iree_arena_allocator_t* arena,
    loom_condition_relation_matrix_view_t* out_view) {
  *out_view = (loom_condition_relation_matrix_view_t){0};
  uint32_t live_row_count = 0;
  uint32_t range_count = 0;
  loom_condition_relation_matrix_count_publication(source, &live_row_count,
                                                   &range_count);
  if (live_row_count == 0) {
    return iree_ok_status();
  }
  const uint64_t sparse_bytes =
      (uint64_t)live_row_count * sizeof(loom_condition_relation_matrix_row_t);
  const uint64_t range_bytes =
      (uint64_t)range_count * sizeof(loom_condition_relation_matrix_range_t);
  return range_bytes < sparse_bytes
             ? loom_condition_relation_matrix_publish_ranges(
                   source, range_count, arena, out_view)
             : loom_condition_relation_matrix_publish_sparse(
                   source, live_row_count, arena, out_view);
}

const loom_condition_relation_set_id_t*
loom_condition_relation_matrix_view_find(
    const loom_condition_relation_matrix_view_t* view, uint32_t left) {
  uint32_t begin = 0;
  uint32_t end = view->entry_count;
  if (view->encoding == LOOM_CONDITION_RELATION_MATRIX_VIEW_SPARSE) {
    while (begin < end) {
      const uint32_t middle = begin + (end - begin) / 2;
      if (view->entries.rows[middle].left < left) {
        begin = middle + 1;
      } else {
        end = middle;
      }
    }
    return begin < view->entry_count && view->entries.rows[begin].left == left
               ? view->entries.rows[begin].excluded
               : NULL;
  }

  if (view->encoding == LOOM_CONDITION_RELATION_MATRIX_VIEW_PAGES) {
    while (begin < end) {
      const uint32_t middle = begin + (end - begin) / 2;
      if (view->entries.pages[middle]->first_left <= left) {
        begin = middle + 1;
      } else {
        end = middle;
      }
    }
    if (begin == 0) {
      return NULL;
    }
    const loom_condition_relation_matrix_page_t* page =
        view->entries.pages[begin - 1];
    return left <= page->last_left
               ? loom_condition_relation_matrix_view_find(&page->contents, left)
               : NULL;
  }

  while (begin < end) {
    const uint32_t middle = begin + (end - begin) / 2;
    if (view->entries.ranges[middle].first_left <= left) {
      begin = middle + 1;
    } else {
      end = middle;
    }
  }
  if (begin == 0) {
    return NULL;
  }
  const loom_condition_relation_matrix_range_t* range =
      &view->entries.ranges[begin - 1];
  return left - range->first_left < range->left_count ? range->excluded : NULL;
}
