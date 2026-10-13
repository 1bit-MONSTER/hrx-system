// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/target/reporting/memory_summary.h"

#include <string.h>

#include "loom/target/reporting/row_list.h"
#include "loom/util/adaptive_sort.h"
#include "loom/util/segmented_storage.h"

// Lookup links are scoped to one function's batch; summary storage and order
// belong to the report. No index bytes survive completion of the batch.
typedef struct loom_report_memory_group_t {
  // Stable retained summary, with a key that does not change while
  // accumulating.
  void* summary;
  // Ordered subtrees for the complete summary key.
  struct loom_report_memory_group_t* children[2];
  // AVL height; empty links have height zero.
  uint8_t height;
} loom_report_memory_group_t;

static uint8_t loom_report_memory_group_height(
    const loom_report_memory_group_t* group) {
  return group ? group->height : 0;
}

static void loom_report_memory_group_update_height(
    loom_report_memory_group_t* group) {
  group->height =
      1 + iree_max(loom_report_memory_group_height(group->children[0]),
                   loom_report_memory_group_height(group->children[1]));
}

static loom_report_memory_group_t* loom_report_memory_group_rotate(
    loom_report_memory_group_t* group, uint32_t direction) {
  loom_report_memory_group_t* pivot = group->children[direction];
  group->children[direction] = pivot->children[1 - direction];
  pivot->children[1 - direction] = group;
  loom_report_memory_group_update_height(group);
  loom_report_memory_group_update_height(pivot);
  return pivot;
}

static loom_report_memory_group_t* loom_report_memory_group_balance(
    loom_report_memory_group_t* group) {
  loom_report_memory_group_update_height(group);
  const int balance = (int)loom_report_memory_group_height(group->children[0]) -
                      (int)loom_report_memory_group_height(group->children[1]);
  if (balance < -1 || balance > 1) {
    const uint32_t direction = balance < 0;
    loom_report_memory_group_t* child = group->children[direction];
    if (loom_report_memory_group_height(child->children[direction]) <
        loom_report_memory_group_height(child->children[1 - direction])) {
      group->children[direction] =
          loom_report_memory_group_rotate(child, 1 - direction);
    }
    group = loom_report_memory_group_rotate(group, direction);
  }
  return group;
}

typedef struct loom_report_memory_group_path_t {
  // AVL height is below twice log2(node_count + 1), bounded by addressable
  // rows.
  loom_report_memory_group_t** links[2 * sizeof(iree_host_size_t) * CHAR_BIT];
  // Number of ancestor links requiring insertion repair.
  iree_host_size_t count;
} loom_report_memory_group_path_t;

static loom_report_memory_group_t** loom_report_memory_group_find(
    loom_report_memory_group_t** root, const void* key,
    int (*compare)(const void*, const void*),
    loom_report_memory_group_path_t* path) {
  path->count = 0;
  loom_report_memory_group_t** link = root;
  while (*link != NULL) {
    const int order = compare(key, (*link)->summary);
    if (order == 0) {
      break;
    }
    path->links[path->count++] = link;
    link = &(*link)->children[order > 0];
  }
  return link;
}

static iree_status_t loom_report_memory_group_insert(
    iree_arena_allocator_t* arena, loom_report_memory_group_t** link,
    loom_report_memory_group_path_t* path, void* summary) {
  loom_report_memory_group_t* group = NULL;
  IREE_RETURN_IF_ERROR(
      iree_arena_allocate(arena, sizeof(*group), (void**)&group));
  *group = (loom_report_memory_group_t){.summary = summary, .height = 1};
  *link = group;
  while (path->count != 0) {
    link = path->links[--path->count];
    const uint8_t old_height = (*link)->height;
    *link = loom_report_memory_group_balance(*link);
    if ((*link)->height == old_height) {
      break;
    }
  }
  return iree_ok_status();
}

static iree_status_t loom_report_memory_group_get_or_append(
    loom_target_compile_report_memory_batch_t* batch,
    loom_report_memory_group_index_t* index,
    loom_target_compile_report_row_list_t* summaries, iree_host_size_t row_size,
    const void* key, int (*compare)(const void*, const void*),
    void** out_summary) {
  if (index->root == NULL) {
    for (uint8_t i = 0; i < index->count; ++i) {
      if (compare(key, index->summaries[i]) == 0) {
        *out_summary = index->summaries[i];
        return iree_ok_status();
      }
    }
    if (index->count < IREE_ARRAYSIZE(index->summaries)) {
      IREE_RETURN_IF_ERROR(loom_target_compile_report_row_list_append(
          summaries, row_size, batch->report->allocator, key));
      void* summary =
          (uint8_t*)loom_target_compile_report_vec_rows(summaries->tail) +
          (summaries->tail->count - 1) * row_size;
      index->summaries[index->count++] = summary;
      *out_summary = summary;
      return iree_ok_status();
    }
    // Promotion visits only the fixed inline capacity, never a report-wide
    // list.
    for (uint8_t i = 0; i < index->count; ++i) {
      loom_report_memory_group_path_t path;
      loom_report_memory_group_t** link = loom_report_memory_group_find(
          &index->root, index->summaries[i], compare, &path);
      IREE_RETURN_IF_ERROR(loom_report_memory_group_insert(
          batch->arena, link, &path, index->summaries[i]));
    }
  }
  loom_report_memory_group_path_t path;
  loom_report_memory_group_t** link =
      loom_report_memory_group_find(&index->root, key, compare, &path);
  if (*link != NULL) {
    *out_summary = (*link)->summary;
    return iree_ok_status();
  }
  IREE_RETURN_IF_ERROR(loom_target_compile_report_row_list_append(
      summaries, row_size, batch->report->allocator, key));
  void* summary =
      (uint8_t*)loom_target_compile_report_vec_rows(summaries->tail) +
      (summaries->tail->count - 1) * row_size;
  IREE_RETURN_IF_ERROR(
      loom_report_memory_group_insert(batch->arena, link, &path, summary));
  *out_summary = summary;
  return iree_ok_status();
}

static bool loom_target_compile_report_memory_interval_has_envelope(
    const loom_target_compile_report_memory_interval_t* interval) {
  const loom_target_compile_report_memory_interval_flags_t range_flags =
      LOOM_TARGET_COMPILE_REPORT_MEMORY_INTERVAL_BEGIN_RANGE |
      LOOM_TARGET_COMPILE_REPORT_MEMORY_INTERVAL_END_RANGE;
  return iree_all_bits_set(interval->flags, range_flags) &&
         interval->end_max_bytes >= interval->begin_min_bytes;
}

static uint64_t loom_target_compile_report_memory_interval_span(
    int64_t begin_bytes, int64_t end_bytes) {
  return end_bytes >= begin_bytes ? (uint64_t)end_bytes - (uint64_t)begin_bytes
                                  : 0;
}

typedef enum loom_target_compile_report_memory_interval_unique_kind_e {
  LOOM_TARGET_COMPILE_REPORT_MEMORY_INTERVAL_UNIQUE_NONE = 0,
  LOOM_TARGET_COMPILE_REPORT_MEMORY_INTERVAL_UNIQUE_STATIC = 1,
  LOOM_TARGET_COMPILE_REPORT_MEMORY_INTERVAL_UNIQUE_SYMBOLIC = 2,
} loom_target_compile_report_memory_interval_unique_kind_t;

typedef struct loom_target_compile_report_memory_interval_unique_delta_t {
  // Proof permitting this packet's contribution, or none for ambiguity.
  loom_target_compile_report_memory_interval_unique_kind_t kind;
  // Previously uncovered bytes proved by this packet.
  uint64_t unique_byte_delta;
} loom_target_compile_report_memory_interval_unique_delta_t;

typedef enum loom_report_memory_direction_e {
  LOOM_REPORT_MEMORY_ALL = 0,
  LOOM_REPORT_MEMORY_READ = 1,
  LOOM_REPORT_MEMORY_WRITE = 2,
} loom_report_memory_direction_t;

typedef struct loom_report_memory_entry_t {
  // Stable retained packet with the report's existing borrowed-string lifetime.
  const loom_target_compile_report_source_low_memory_row_t* row;
  // Root summary established while recording this packet.
  loom_target_compile_report_source_low_memory_root_summary_t* root;
  // Argument summary, or NULL for a non-argument root.
  loom_target_compile_report_source_low_memory_argument_summary_t* argument;
  // Original packet order within the batch's eligible interval evidence.
  iree_host_size_t ordinal;
  // Temporary grouping or expression-pair key used only while sorting.
  uint64_t sort_key;
  // Coordinate ranks of the inclusive start and exclusive end.
  struct {
    // Inclusive start rank.
    iree_host_size_t begin;
    // Exclusive end rank.
    iree_host_size_t end;
  } span;
  // First identical expression pair in each applicable encounter sequence.
  struct {
    // First packet across all operations, or NULL for the first occurrence.
    const struct loom_report_memory_entry_t* overall;
    // First packet with the same read/write direction, or NULL.
    const struct loom_report_memory_entry_t* direction;
  } duplicates;
  // Static or exact-symbolic evidence; imprecise intervals are not indexed.
  loom_target_compile_report_memory_interval_unique_kind_t kind;
  // Read/write contribution, or ALL for other operation kinds.
  loom_report_memory_direction_t direction;
} loom_report_memory_entry_t;

// The earliest witness either survives exclusion of one begin and one end ID,
// or matches an excluded ID. If its begin is excluded, the first different
// begin is sufficient unless its end is also excluded; the next witness must
// then differ from both. The symmetric case handles an excluded end. Five
// witnesses therefore answer this query exactly, independent of packet count.
typedef struct loom_report_memory_witnesses_t {
  // Earliest packet, or NULL for an empty set.
  const loom_report_memory_entry_t* first;
  // Earliest witnesses whose begin differs from first's begin.
  struct {
    // First such packet.
    const loom_report_memory_entry_t* first;
    // First such packet whose end also differs from this group's first.
    const loom_report_memory_entry_t* different_end;
  } different_begin;
  // Earliest witnesses whose end differs from first's end.
  struct {
    // First such packet.
    const loom_report_memory_entry_t* first;
    // First such packet whose begin also differs from this group's first.
    const loom_report_memory_entry_t* different_begin;
  } different_end;
} loom_report_memory_witnesses_t;

typedef struct loom_report_memory_node_t {
  // Witnesses covering this entire coordinate range.
  loom_report_memory_witnesses_t local;
  // Witnesses intersecting any part of this coordinate range.
  loom_report_memory_witnesses_t subtree;
  // Bytes covered by static intervals; full coverage follows from range size.
  uint64_t covered_bytes;
} loom_report_memory_node_t;

enum {
  // 32 nodes fit comfortably within one pooled 4 KiB allocation.
  LOOM_REPORT_MEMORY_NODE_BLOCK_SHIFT = 5,
  LOOM_REPORT_MEMORY_NODE_BLOCK_CAPACITY =
      1 << LOOM_REPORT_MEMORY_NODE_BLOCK_SHIFT,
  // Coordinate and packet-pointer blocks each fit within 4 KiB.
  LOOM_REPORT_MEMORY_WORD_BLOCK_SHIFT = 9,
  LOOM_REPORT_MEMORY_WORD_BLOCK_CAPACITY =
      1 << LOOM_REPORT_MEMORY_WORD_BLOCK_SHIFT,
};

typedef struct loom_report_memory_tree_t {
  // Sorted distinct byte boundaries, biased into unsigned coordinate order.
  loom_segmented_storage_t* coordinates;
  // Number of coordinate intervals (one less than the boundary count).
  iree_host_size_t segment_count;
  // Fixed-capacity node blocks owned by the current root's scratch arena.
  loom_segmented_storage_t nodes;
} loom_report_memory_tree_t;

typedef loom_report_memory_entry_t* loom_report_memory_entry_ptr_t;

// A sortable slice of packet references; the original rows remain stable.
typedef struct loom_report_memory_entries_t {
  // Block storage shared by the batch and its per-root slices.
  loom_segmented_storage_t* storage;
  // First packet reference in this slice.
  iree_host_size_t offset;
} loom_report_memory_entries_t;

static iree_status_t loom_report_memory_allocate_segments(
    loom_segmented_storage_t* storage, iree_host_size_t count,
    iree_arena_allocator_t* arena) {
  iree_status_t status = iree_ok_status();
  while (storage->segment_count < count && iree_status_is_ok(status)) {
    void* segment = NULL;
    status = loom_segmented_storage_append(storage, arena, &segment);
  }
  return status;
}

static uint64_t* loom_report_memory_coordinate_at(
    loom_segmented_storage_t* storage, iree_host_size_t index) {
  uint64_t* block = (uint64_t*)loom_segmented_storage_segment(
      storage, index >> LOOM_REPORT_MEMORY_WORD_BLOCK_SHIFT);
  return &block[index & (LOOM_REPORT_MEMORY_WORD_BLOCK_CAPACITY - 1)];
}

static loom_report_memory_entry_ptr_t* loom_report_memory_entry_at(
    loom_report_memory_entries_t entries, iree_host_size_t index) {
  index += entries.offset;
  loom_report_memory_entry_ptr_t* block =
      (loom_report_memory_entry_ptr_t*)loom_segmented_storage_segment(
          entries.storage, index >> LOOM_REPORT_MEMORY_WORD_BLOCK_SHIFT);
  return &block[index & (LOOM_REPORT_MEMORY_WORD_BLOCK_CAPACITY - 1)];
}

// Static packets use the reserved no-expression ID. Static queries exclude
// them from ambiguity witnesses and use the byte union instead; symbolic
// queries still see them as possible conflicts.
static uint32_t loom_report_memory_begin_id(
    const loom_report_memory_entry_t* entry) {
  return entry->kind == LOOM_TARGET_COMPILE_REPORT_MEMORY_INTERVAL_UNIQUE_STATIC
             ? UINT32_MAX
             : entry->row->source_interval.begin_expr_id;
}

static uint32_t loom_report_memory_end_id(
    const loom_report_memory_entry_t* entry) {
  return entry->kind == LOOM_TARGET_COMPILE_REPORT_MEMORY_INTERVAL_UNIQUE_STATIC
             ? UINT32_MAX
             : entry->row->source_interval.end_expr_id;
}

static void loom_report_memory_witnesses_append(
    loom_report_memory_witnesses_t* witnesses,
    const loom_report_memory_entry_t* entry) {
  if (witnesses->first == NULL) {
    witnesses->first = entry;
    return;
  }
  // Packets arrive in increasing ordinal order, so each witness is filled once.
  if (loom_report_memory_begin_id(entry) !=
      loom_report_memory_begin_id(witnesses->first)) {
    if (witnesses->different_begin.first == NULL) {
      witnesses->different_begin.first = entry;
    } else if (witnesses->different_begin.different_end == NULL &&
               loom_report_memory_end_id(entry) !=
                   loom_report_memory_end_id(
                       witnesses->different_begin.first)) {
      witnesses->different_begin.different_end = entry;
    }
  }
  if (loom_report_memory_end_id(entry) !=
      loom_report_memory_end_id(witnesses->first)) {
    if (witnesses->different_end.first == NULL) {
      witnesses->different_end.first = entry;
    } else if (witnesses->different_end.different_begin == NULL &&
               loom_report_memory_begin_id(entry) !=
                   loom_report_memory_begin_id(
                       witnesses->different_end.first)) {
      witnesses->different_end.different_begin = entry;
    }
  }
}

static const loom_report_memory_entry_t* loom_report_memory_witnesses_query(
    const loom_report_memory_witnesses_t* witnesses, uint32_t excluded_begin,
    uint32_t excluded_end) {
  if (witnesses->first == NULL) {
    return NULL;
  }
  if (loom_report_memory_begin_id(witnesses->first) == excluded_begin) {
    const loom_report_memory_entry_t* first = witnesses->different_begin.first;
    return first == NULL || loom_report_memory_end_id(first) != excluded_end
               ? first
               : witnesses->different_begin.different_end;
  }
  if (loom_report_memory_end_id(witnesses->first) == excluded_end) {
    const loom_report_memory_entry_t* first = witnesses->different_end.first;
    return first == NULL || loom_report_memory_begin_id(first) != excluded_begin
               ? first
               : witnesses->different_end.different_begin;
  }
  return witnesses->first;
}

static const loom_report_memory_entry_t* loom_report_memory_first(
    const loom_report_memory_entry_t* lhs,
    const loom_report_memory_entry_t* rhs) {
  return rhs != NULL && (lhs == NULL || rhs->ordinal < lhs->ordinal) ? rhs
                                                                     : lhs;
}

static loom_report_memory_node_t* loom_report_memory_node(
    loom_report_memory_tree_t* tree, iree_host_size_t index) {
  loom_report_memory_node_t* block =
      (loom_report_memory_node_t*)loom_segmented_storage_segment(
          &tree->nodes, index >> LOOM_REPORT_MEMORY_NODE_BLOCK_SHIFT);
  return &block[index & (LOOM_REPORT_MEMORY_NODE_BLOCK_CAPACITY - 1)];
}

static const loom_report_memory_entry_t* loom_report_memory_query(
    loom_report_memory_tree_t* tree, iree_host_size_t index,
    iree_host_size_t begin, iree_host_size_t end,
    const loom_report_memory_entry_t* entry) {
  const loom_report_memory_node_t* node = loom_report_memory_node(tree, index);
  const uint32_t excluded_begin = loom_report_memory_end_id(entry);
  const uint32_t excluded_end = loom_report_memory_begin_id(entry);
  if (entry->span.begin <= begin && entry->span.end >= end) {
    return loom_report_memory_witnesses_query(&node->subtree, excluded_begin,
                                              excluded_end);
  }
  const loom_report_memory_entry_t* first = loom_report_memory_witnesses_query(
      &node->local, excluded_begin, excluded_end);
  const iree_host_size_t middle = begin + (end - begin) / 2;
  if (entry->span.begin < middle) {
    first = loom_report_memory_first(
        first, loom_report_memory_query(tree, index + 1, begin, middle, entry));
  }
  if (entry->span.end > middle) {
    // Preorder storage has exactly two nodes per segment except the final leaf.
    first = loom_report_memory_first(
        first, loom_report_memory_query(tree, index + 2 * (middle - begin),
                                        middle, end, entry));
  }
  return first;
}

static uint64_t loom_report_memory_insert(
    loom_report_memory_tree_t* tree, iree_host_size_t index,
    iree_host_size_t begin, iree_host_size_t end,
    const loom_report_memory_entry_t* entry) {
  loom_report_memory_node_t* node = loom_report_memory_node(tree, index);
  const uint64_t length =
      *loom_report_memory_coordinate_at(tree->coordinates, end) -
      *loom_report_memory_coordinate_at(tree->coordinates, begin);
  const bool is_static =
      entry->kind == LOOM_TARGET_COMPILE_REPORT_MEMORY_INTERVAL_UNIQUE_STATIC;
  if (is_static && node->covered_bytes == length) {
    // Earlier static packets already cover every byte and are earlier blockers.
    return 0;
  }
  loom_report_memory_witnesses_append(&node->subtree, entry);
  if (entry->span.begin <= begin && entry->span.end >= end) {
    loom_report_memory_witnesses_append(&node->local, entry);
    if (!is_static) {
      return 0;
    }
    const uint64_t added = length - node->covered_bytes;
    node->covered_bytes = length;
    return added;
  }
  const iree_host_size_t middle = begin + (end - begin) / 2;
  uint64_t added = 0;
  if (entry->span.begin < middle) {
    added += loom_report_memory_insert(tree, index + 1, begin, middle, entry);
  }
  if (entry->span.end > middle) {
    added += loom_report_memory_insert(tree, index + 2 * (middle - begin),
                                       middle, end, entry);
  }
  node->covered_bytes += added;
  return added;
}

static bool loom_report_memory_entry_less(
    void* context, const loom_report_memory_entry_ptr_t* lhs,
    const loom_report_memory_entry_ptr_t* rhs) {
  (void)context;
  return (*lhs)->sort_key < (*rhs)->sort_key ||
         ((*lhs)->sort_key == (*rhs)->sort_key &&
          (*lhs)->ordinal < (*rhs)->ordinal);
}
LOOM_DEFINE_ADAPTIVE_SORT_WITH_ACCESSOR(loom_report_memory_sort_entries,
                                        loom_report_memory_entry_ptr_t,
                                        loom_report_memory_entries_t,
                                        loom_report_memory_entry_at, void*,
                                        loom_report_memory_entry_less)

static bool loom_report_memory_coordinate_less(void* context,
                                               const uint64_t* lhs,
                                               const uint64_t* rhs) {
  (void)context;
  return *lhs < *rhs;
}
LOOM_DEFINE_ADAPTIVE_SORT_WITH_ACCESSOR(loom_report_memory_sort_coordinates,
                                        uint64_t, loom_segmented_storage_t*,
                                        loom_report_memory_coordinate_at, void*,
                                        loom_report_memory_coordinate_less)

static uint64_t loom_report_memory_coordinate(int64_t offset) {
  return (uint64_t)offset ^ (UINT64_C(1) << 63);
}

static iree_host_size_t loom_report_memory_coordinate_rank(
    loom_segmented_storage_t* coordinates, iree_host_size_t count,
    uint64_t coordinate) {
  iree_host_size_t begin = 0;
  while (begin < count) {
    const iree_host_size_t middle = begin + (count - begin) / 2;
    if (*loom_report_memory_coordinate_at(coordinates, middle) < coordinate) {
      begin = middle + 1;
    } else {
      count = middle;
    }
  }
  return begin;
}

static void loom_report_memory_accumulate_unique(
    loom_target_compile_report_source_low_memory_summary_t* summary,
    loom_report_memory_direction_t direction,
    loom_target_compile_report_memory_interval_unique_delta_t delta) {
  loom_target_compile_report_memory_interval_summary_t* interval =
      direction == LOOM_REPORT_MEMORY_READ ? &summary->read_interval_envelope
      : direction == LOOM_REPORT_MEMORY_WRITE
          ? &summary->write_interval_envelope
          : &summary->interval_envelope;
  interval->exact_static_packet_count +=
      delta.kind == LOOM_TARGET_COMPILE_REPORT_MEMORY_INTERVAL_UNIQUE_STATIC;
  interval->exact_symbolic_packet_count +=
      delta.kind == LOOM_TARGET_COMPILE_REPORT_MEMORY_INTERVAL_UNIQUE_SYMBOLIC;
  interval->unique_byte_count += delta.unique_byte_delta;
}

static void loom_report_memory_apply_unique(
    const loom_report_memory_entry_t* entry,
    loom_report_memory_direction_t direction,
    loom_target_compile_report_memory_interval_unique_delta_t delta) {
  loom_report_memory_accumulate_unique(&entry->root->summary, direction, delta);
  if (entry->argument != NULL) {
    loom_report_memory_accumulate_unique(&entry->argument->summary, direction,
                                         delta);
  }
}

static iree_status_t loom_report_memory_finish_root(
    loom_report_memory_entries_t entries, iree_host_size_t count,
    iree_arena_allocator_t* arena) {
  if (count == 1) {
    const loom_report_memory_entry_t* entry =
        *loom_report_memory_entry_at(entries, 0);
    const loom_target_compile_report_memory_interval_t* interval =
        &entry->row->source_interval;
    const loom_target_compile_report_memory_interval_unique_delta_t delta = {
        .kind = entry->kind,
        .unique_byte_delta =
            entry->kind ==
                    LOOM_TARGET_COMPILE_REPORT_MEMORY_INTERVAL_UNIQUE_STATIC
                ? (uint64_t)interval->end_max_bytes -
                      (uint64_t)interval->begin_min_bytes
                : interval->exact_length_bytes,
    };
    loom_report_memory_apply_unique(entry, LOOM_REPORT_MEMORY_ALL, delta);
    if (entry->direction != LOOM_REPORT_MEMORY_ALL) {
      loom_report_memory_apply_unique(entry, entry->direction, delta);
    }
    return iree_ok_status();
  }

  loom_segmented_storage_t coordinates;
  loom_segmented_storage_initialize(
      LOOM_REPORT_MEMORY_WORD_BLOCK_CAPACITY * sizeof(uint64_t),
      iree_alignof(uint64_t), &coordinates);
  IREE_RETURN_IF_ERROR(loom_report_memory_allocate_segments(
      &coordinates,
      iree_host_size_ceil_div(count * 2,
                              LOOM_REPORT_MEMORY_WORD_BLOCK_CAPACITY),
      arena));
  for (iree_host_size_t i = 0; i < count; ++i) {
    loom_report_memory_entry_t* entry =
        *loom_report_memory_entry_at(entries, i);
    const loom_target_compile_report_memory_interval_t* interval =
        &entry->row->source_interval;
    *loom_report_memory_coordinate_at(&coordinates, 2 * i) =
        loom_report_memory_coordinate(interval->begin_min_bytes);
    *loom_report_memory_coordinate_at(&coordinates, 2 * i + 1) =
        loom_report_memory_coordinate(interval->end_max_bytes);
    entry->sort_key = ((uint64_t)loom_report_memory_begin_id(entry) << 32) |
                      loom_report_memory_end_id(entry);
  }
  loom_report_memory_sort_coordinates(NULL, &coordinates, count * 2);
  iree_host_size_t coordinate_count = 1;
  for (iree_host_size_t i = 1; i < count * 2; ++i) {
    const uint64_t coordinate =
        *loom_report_memory_coordinate_at(&coordinates, i);
    if (coordinate !=
        *loom_report_memory_coordinate_at(&coordinates, coordinate_count - 1)) {
      *loom_report_memory_coordinate_at(&coordinates, coordinate_count++) =
          coordinate;
    }
  }

  // Equal symbolic endpoints deduplicate even when their range facts differ.
  // Retain this fact before returning to encounter order; the range query alone
  // cannot find duplicates whose refined envelopes happen to be disjoint.
  loom_report_memory_sort_entries(NULL, entries, count);
  const loom_report_memory_entry_t* first = NULL;
  const loom_report_memory_entry_t* first_direction[3] = {NULL, NULL, NULL};
  uint32_t direction_mask = 1u << LOOM_REPORT_MEMORY_ALL;
  for (iree_host_size_t i = 0; i < count; ++i) {
    loom_report_memory_entry_t* entry =
        *loom_report_memory_entry_at(entries, i);
    const loom_target_compile_report_memory_interval_t* interval =
        &entry->row->source_interval;
    entry->span.begin = loom_report_memory_coordinate_rank(
        &coordinates, coordinate_count,
        loom_report_memory_coordinate(interval->begin_min_bytes));
    entry->span.end = loom_report_memory_coordinate_rank(
        &coordinates, coordinate_count,
        loom_report_memory_coordinate(interval->end_max_bytes));
    direction_mask |= 1u << entry->direction;
    if (entry->kind ==
        LOOM_TARGET_COMPILE_REPORT_MEMORY_INTERVAL_UNIQUE_STATIC) {
      continue;
    }
    if (first == NULL || first->sort_key != entry->sort_key) {
      first = NULL;
      first_direction[LOOM_REPORT_MEMORY_ALL] = NULL;
      first_direction[LOOM_REPORT_MEMORY_READ] = NULL;
      first_direction[LOOM_REPORT_MEMORY_WRITE] = NULL;
    }
    entry->duplicates.overall = first;
    entry->duplicates.direction = first_direction[entry->direction];
    if (first == NULL) {
      first = entry;
    }
    if (first_direction[entry->direction] == NULL) {
      first_direction[entry->direction] = entry;
    }
  }
  for (iree_host_size_t i = 0; i < count; ++i) {
    (*loom_report_memory_entry_at(entries, i))->sort_key = 0;
  }
  loom_report_memory_sort_entries(NULL, entries, count);

  loom_report_memory_tree_t tree = {
      .coordinates = &coordinates,
      .segment_count = coordinate_count - 1,
  };
  loom_segmented_storage_initialize(LOOM_REPORT_MEMORY_NODE_BLOCK_CAPACITY *
                                        sizeof(loom_report_memory_node_t),
                                    iree_alignof(loom_report_memory_node_t),
                                    &tree.nodes);
  IREE_RETURN_IF_ERROR(loom_report_memory_allocate_segments(
      &tree.nodes,
      iree_host_size_ceil_div(2 * tree.segment_count - 1,
                              LOOM_REPORT_MEMORY_NODE_BLOCK_CAPACITY),
      arena));
  for (loom_report_memory_direction_t direction = LOOM_REPORT_MEMORY_ALL;
       direction <= LOOM_REPORT_MEMORY_WRITE; ++direction) {
    if ((direction_mask & (1u << direction)) == 0) {
      continue;
    }
    for (uint32_t i = 0; i < tree.nodes.segment_count; ++i) {
      memset(loom_segmented_storage_segment(&tree.nodes, i), 0,
             tree.nodes.segment_size);
    }
    for (iree_host_size_t i = 0; i < count; ++i) {
      const loom_report_memory_entry_t* entry =
          *loom_report_memory_entry_at(entries, i);
      if (direction != LOOM_REPORT_MEMORY_ALL &&
          entry->direction != direction) {
        continue;
      }
      const loom_target_compile_report_memory_interval_t* interval =
          &entry->row->source_interval;
      const loom_report_memory_entry_t* prior =
          loom_report_memory_query(&tree, 0, 0, tree.segment_count, entry);
      prior =
          loom_report_memory_first(prior, direction == LOOM_REPORT_MEMORY_ALL
                                              ? entry->duplicates.overall
                                              : entry->duplicates.direction);
      const uint64_t added =
          loom_report_memory_insert(&tree, 0, 0, tree.segment_count, entry);
      loom_target_compile_report_memory_interval_unique_delta_t delta = {0};
      if (prior == NULL) {
        delta.kind = entry->kind;
        delta.unique_byte_delta =
            entry->kind ==
                    LOOM_TARGET_COMPILE_REPORT_MEMORY_INTERVAL_UNIQUE_STATIC
                ? added
                : interval->exact_length_bytes;
      } else if (
          entry->kind ==
              LOOM_TARGET_COMPILE_REPORT_MEMORY_INTERVAL_UNIQUE_SYMBOLIC &&
          prior->kind == entry->kind &&
          loom_report_memory_begin_id(prior) == interval->begin_expr_id &&
          loom_report_memory_end_id(prior) == interval->end_expr_id) {
        delta.kind = entry->kind;
      }
      loom_report_memory_apply_unique(entry, direction, delta);
    }
  }
  return iree_ok_status();
}

void loom_target_compile_report_memory_batch_initialize(
    loom_target_compile_report_t* report, iree_arena_allocator_t* arena,
    loom_target_compile_report_memory_batch_t* out_batch) {
  *out_batch = (loom_target_compile_report_memory_batch_t){
      .report = report,
      .arena = arena,
  };
}

iree_status_t loom_target_compile_report_memory_batch_finish(
    loom_target_compile_report_memory_batch_t* batch) {
  if (batch->entries.count == 0) {
    return iree_ok_status();
  }
  loom_segmented_storage_t storage;
  loom_segmented_storage_initialize(
      LOOM_REPORT_MEMORY_WORD_BLOCK_CAPACITY *
          sizeof(loom_report_memory_entry_ptr_t),
      iree_alignof(loom_report_memory_entry_ptr_t), &storage);
  IREE_RETURN_IF_ERROR(loom_report_memory_allocate_segments(
      &storage,
      iree_host_size_ceil_div(batch->entries.count,
                              LOOM_REPORT_MEMORY_WORD_BLOCK_CAPACITY),
      batch->arena));
  const loom_report_memory_entries_t entries = {.storage = &storage};
  iree_host_size_t count = 0;
  for (loom_target_compile_report_vec_t* block = batch->entries.head;
       block != NULL; block = block->next) {
    loom_report_memory_entry_t* rows =
        (loom_report_memory_entry_t*)loom_target_compile_report_vec_rows(block);
    for (iree_host_size_t i = 0; i < block->count; ++i) {
      rows[i].sort_key = (uintptr_t)rows[i].root;
      *loom_report_memory_entry_at(entries, count++) = &rows[i];
    }
  }
  loom_report_memory_sort_entries(NULL, entries, count);
  iree_arena_allocator_t group_arena;
  iree_arena_initialize(batch->arena->block_pool, &group_arena);
  iree_status_t status = iree_ok_status();
  iree_host_size_t begin = 0;
  while (begin < count && iree_status_is_ok(status)) {
    iree_host_size_t end = begin + 1;
    const loom_report_memory_entry_t* first =
        *loom_report_memory_entry_at(entries, begin);
    while (end < count &&
           (*loom_report_memory_entry_at(entries, end))->root == first->root) {
      ++end;
    }
    const loom_report_memory_entries_t root_entries = {
        .storage = &storage,
        .offset = begin,
    };
    status =
        loom_report_memory_finish_root(root_entries, end - begin, &group_arena);
    iree_arena_reset(&group_arena);
    begin = end;
  }
  iree_arena_deinitialize(&group_arena);
  return status;
}

static bool loom_target_compile_report_memory_interval_is_exact_static(
    const loom_target_compile_report_memory_interval_t* interval) {
  return loom_target_compile_report_memory_interval_has_envelope(interval) &&
         interval->begin_min_bytes == interval->begin_max_bytes &&
         interval->end_min_bytes == interval->end_max_bytes;
}

static bool loom_target_compile_report_memory_interval_has_exact_symbolic(
    const loom_target_compile_report_memory_interval_t* interval) {
  const loom_target_compile_report_memory_interval_flags_t required_flags =
      LOOM_TARGET_COMPILE_REPORT_MEMORY_INTERVAL_BEGIN_RANGE |
      LOOM_TARGET_COMPILE_REPORT_MEMORY_INTERVAL_END_RANGE |
      LOOM_TARGET_COMPILE_REPORT_MEMORY_INTERVAL_EXACT_LENGTH |
      LOOM_TARGET_COMPILE_REPORT_MEMORY_INTERVAL_BEGIN_EXPR |
      LOOM_TARGET_COMPILE_REPORT_MEMORY_INTERVAL_END_EXPR;
  return iree_all_bits_set(interval->flags, required_flags) &&
         interval->exact_length_bytes != 0;
}

static void loom_target_compile_report_merge_memory_interval_envelope_bounds(
    loom_target_compile_report_memory_interval_summary_t* target,
    int64_t begin_min_bytes, int64_t end_max_bytes) {
  if (target->packet_count == 0) {
    target->envelope_begin_min_bytes = begin_min_bytes;
    target->envelope_end_max_bytes = end_max_bytes;
  } else {
    target->envelope_begin_min_bytes =
        iree_min(target->envelope_begin_min_bytes, begin_min_bytes);
    target->envelope_end_max_bytes =
        iree_max(target->envelope_end_max_bytes, end_max_bytes);
  }
  target->envelope_byte_count = loom_target_compile_report_memory_interval_span(
      target->envelope_begin_min_bytes, target->envelope_end_max_bytes);
}

static void loom_target_compile_report_accumulate_memory_interval_summary(
    loom_target_compile_report_memory_interval_summary_t* target,
    const loom_target_compile_report_memory_interval_t* interval) {
  if (!loom_target_compile_report_memory_interval_has_envelope(interval)) {
    return;
  }
  loom_target_compile_report_merge_memory_interval_envelope_bounds(
      target, interval->begin_min_bytes, interval->end_max_bytes);
  ++target->packet_count;
}

static void loom_target_compile_report_merge_memory_interval_envelope_summary(
    loom_target_compile_report_memory_interval_summary_t* target,
    const loom_target_compile_report_memory_interval_summary_t* source) {
  if (source->packet_count == 0) {
    return;
  }
  loom_target_compile_report_merge_memory_interval_envelope_bounds(
      target, source->envelope_begin_min_bytes, source->envelope_end_max_bytes);
  target->packet_count += source->packet_count;
}

static void loom_target_compile_report_forget_memory_interval_unique_accounting(
    loom_target_compile_report_memory_interval_summary_t* summary) {
  summary->exact_static_packet_count = 0;
  summary->exact_symbolic_packet_count = 0;
  summary->unique_byte_count = 0;
}

void loom_target_compile_report_accumulate_bank_service_summaries(
    loom_target_compile_report_bank_service_summary_t* target,
    const loom_target_compile_report_bank_service_summary_t* source) {
  target->unmodeled_packet_count += source->unmodeled_packet_count;
  target->modeled_packet_count += source->modeled_packet_count;
  target->exact_packet_count += source->exact_packet_count;
  target->unknown_packet_count += source->unknown_packet_count;
  target->conflict_free_packet_count += source->conflict_free_packet_count;
  target->conflicted_packet_count += source->conflicted_packet_count;
  target->required_round_count += source->required_round_count;
  target->uncontended_round_count += source->uncontended_round_count;
  target->extra_round_count += source->extra_round_count;
  target->exact_dynamic_packet_count += source->exact_dynamic_packet_count;
  target->unknown_dynamic_packet_count += source->unknown_dynamic_packet_count;
  target->dynamic_packet_count += source->dynamic_packet_count;
  target->dynamic_required_round_count += source->dynamic_required_round_count;
  target->dynamic_uncontended_round_count +=
      source->dynamic_uncontended_round_count;
  target->dynamic_extra_round_count += source->dynamic_extra_round_count;
  target->maximum_request_multiplicity =
      iree_max(target->maximum_request_multiplicity,
               source->maximum_request_multiplicity);
}

void loom_target_compile_report_accumulate_subgroup_access_summaries(
    loom_target_compile_report_subgroup_access_summary_t* target,
    const loom_target_compile_report_subgroup_access_summary_t* source) {
  target->modeled_packet_count += source->modeled_packet_count;
  target->exact_packet_count += source->exact_packet_count;
  target->unknown_packet_count += source->unknown_packet_count;
  target->dense_packet_count += source->dense_packet_count;
  target->gapped_packet_count += source->gapped_packet_count;
  target->overlapping_packet_count += source->overlapping_packet_count;
  target->exact_dynamic_packet_count += source->exact_dynamic_packet_count;
  target->unknown_dynamic_packet_count += source->unknown_dynamic_packet_count;
  target->dynamic_packet_count += source->dynamic_packet_count;
  target->dynamic_dense_packet_count += source->dynamic_dense_packet_count;
  target->dynamic_gapped_packet_count += source->dynamic_gapped_packet_count;
  target->dynamic_overlapping_packet_count +=
      source->dynamic_overlapping_packet_count;
}

void loom_target_compile_report_accumulate_source_low_memory_summaries(
    loom_target_compile_report_source_low_memory_summary_t* target,
    const loom_target_compile_report_source_low_memory_summary_t* source) {
  target->packet_count += source->packet_count;
  target->unknown_dynamic_packet_count += source->unknown_dynamic_packet_count;
  target->exact_dynamic_packet_count += source->exact_dynamic_packet_count;
  target->load_packet_count += source->load_packet_count;
  target->store_packet_count += source->store_packet_count;
  target->scalar_packet_count += source->scalar_packet_count;
  target->vector_packet_count += source->vector_packet_count;
  target->source_lane_count += source->source_lane_count;
  target->source_byte_count += source->source_byte_count;
  target->read_byte_count += source->read_byte_count;
  target->write_byte_count += source->write_byte_count;
  target->issued_read_byte_count += source->issued_read_byte_count;
  target->issued_write_byte_count += source->issued_write_byte_count;
  target->issued_read_unknown_width_count +=
      source->issued_read_unknown_width_count;
  target->issued_write_unknown_width_count +=
      source->issued_write_unknown_width_count;
  target->dynamic_packet_count += source->dynamic_packet_count;
  target->dynamic_source_byte_count += source->dynamic_source_byte_count;
  target->dynamic_read_byte_count += source->dynamic_read_byte_count;
  target->dynamic_write_byte_count += source->dynamic_write_byte_count;
  target->dynamic_issued_read_byte_count +=
      source->dynamic_issued_read_byte_count;
  target->dynamic_issued_write_byte_count +=
      source->dynamic_issued_write_byte_count;
  target->dynamic_issued_read_unknown_width_count +=
      source->dynamic_issued_read_unknown_width_count;
  target->dynamic_issued_write_unknown_width_count +=
      source->dynamic_issued_write_unknown_width_count;
  target->contiguous_vector_packet_count +=
      source->contiguous_vector_packet_count;
  target->strided_vector_packet_count += source->strided_vector_packet_count;
  target->unknown_stride_vector_packet_count +=
      source->unknown_stride_vector_packet_count;
  loom_target_compile_report_merge_memory_interval_envelope_summary(
      &target->interval_envelope, &source->interval_envelope);
  loom_target_compile_report_merge_memory_interval_envelope_summary(
      &target->read_interval_envelope, &source->read_interval_envelope);
  loom_target_compile_report_merge_memory_interval_envelope_summary(
      &target->write_interval_envelope, &source->write_interval_envelope);
}

static void loom_target_compile_report_merge_source_low_memory_summary(
    loom_target_compile_report_source_low_memory_summary_t* target,
    const loom_target_compile_report_source_low_memory_summary_t* source) {
  loom_target_compile_report_accumulate_source_low_memory_summaries(target,
                                                                    source);
  loom_target_compile_report_forget_memory_interval_unique_accounting(
      &target->interval_envelope);
  loom_target_compile_report_forget_memory_interval_unique_accounting(
      &target->read_interval_envelope);
  loom_target_compile_report_forget_memory_interval_unique_accounting(
      &target->write_interval_envelope);
}

static void loom_target_compile_report_merge_source_low_memory_root_summary(
    loom_target_compile_report_source_low_memory_root_summary_t* target,
    const loom_target_compile_report_source_low_memory_root_summary_t* source) {
  loom_target_compile_report_merge_source_low_memory_summary(&target->summary,
                                                             &source->summary);
}

static void loom_target_compile_report_merge_source_low_memory_argument_summary(
    loom_target_compile_report_source_low_memory_argument_summary_t* target,
    const loom_target_compile_report_source_low_memory_argument_summary_t*
        source) {
  if (!iree_string_view_is_empty(target->source_root_name) &&
      (iree_string_view_is_empty(source->source_root_name) ||
       !iree_string_view_equal(target->source_root_name,
                               source->source_root_name))) {
    target->source_root_name = iree_string_view_empty();
  }
  loom_target_compile_report_merge_source_low_memory_summary(&target->summary,
                                                             &source->summary);
}

static void
loom_target_compile_report_merge_source_low_memory_argument_packet_summary(
    loom_target_compile_report_source_low_memory_argument_packet_summary_t*
        target,
    const loom_target_compile_report_source_low_memory_argument_packet_summary_t*
        source) {
  if (!iree_string_view_is_empty(target->source_root_name) &&
      (iree_string_view_is_empty(source->source_root_name) ||
       !iree_string_view_equal(target->source_root_name,
                               source->source_root_name))) {
    target->source_root_name = iree_string_view_empty();
  }
  loom_target_compile_report_merge_source_low_memory_summary(&target->summary,
                                                             &source->summary);
}

static void loom_target_compile_report_merge_source_low_memory_strategy_summary(
    loom_target_compile_report_source_low_memory_strategy_summary_t* target,
    const loom_target_compile_report_source_low_memory_strategy_summary_t*
        source) {
  loom_target_compile_report_merge_source_low_memory_summary(&target->summary,
                                                             &source->summary);
}

// Compare only semantic key fields: summaries also carry mutable counters and
// non-key labels, and aggregate padding is not part of their identity.
static int loom_report_memory_string_compare(iree_string_view_t lhs,
                                             iree_string_view_t rhs) {
  if (lhs.size != rhs.size) {
    return lhs.size < rhs.size ? -1 : 1;
  }
  if (lhs.size == 0 || lhs.data == rhs.data) {
    return 0;
  }
  return memcmp(lhs.data, rhs.data, lhs.size);
}

#define LOOM_REPORT_MEMORY_COMPARE_STRING(field)                   \
  do {                                                             \
    const int order =                                              \
        loom_report_memory_string_compare(lhs->field, rhs->field); \
    if (order != 0) {                                              \
      return order;                                                \
    }                                                              \
  } while (0)
#define LOOM_REPORT_MEMORY_COMPARE_SCALAR(field) \
  do {                                           \
    if (lhs->field != rhs->field) {              \
      return lhs->field < rhs->field ? -1 : 1;   \
    }                                            \
  } while (0)

// Keys are ordered within one function namespace. Capture owns that namespace
// through its batch; entry merging checks function equality before ordering
// the remaining fields. Counters and mutable descriptive labels are not keys.
static int loom_report_memory_root_compare(const void* lhs_data,
                                           const void* rhs_data) {
  const loom_target_compile_report_source_low_memory_root_summary_t* lhs =
      lhs_data;
  const loom_target_compile_report_source_low_memory_root_summary_t* rhs =
      rhs_data;
  LOOM_REPORT_MEMORY_COMPARE_STRING(source_root_name);
  LOOM_REPORT_MEMORY_COMPARE_SCALAR(source_root_argument_index);
  LOOM_REPORT_MEMORY_COMPARE_STRING(memory_space);
  return 0;
}

static int loom_report_memory_argument_compare(const void* lhs_data,
                                               const void* rhs_data) {
  const loom_target_compile_report_source_low_memory_argument_summary_t* lhs =
      lhs_data;
  const loom_target_compile_report_source_low_memory_argument_summary_t* rhs =
      rhs_data;
  LOOM_REPORT_MEMORY_COMPARE_SCALAR(source_root_argument_index);
  LOOM_REPORT_MEMORY_COMPARE_STRING(memory_space);
  return 0;
}

static loom_target_compile_report_source_low_memory_root_summary_t*
loom_target_compile_report_find_source_low_memory_root_summary(
    loom_target_compile_report_t* report, iree_string_view_t function_name,
    iree_string_view_t source_root_name, uint16_t source_root_argument_index,
    iree_string_view_t memory_space) {
  const bool has_root_identity = !iree_string_view_is_empty(source_root_name) ||
                                 source_root_argument_index != UINT16_MAX;
  if (!has_root_identity) {
    return NULL;
  }
  const loom_target_compile_report_source_low_memory_root_summary_t key = {
      .function_name = function_name,
      .source_root_name = source_root_name,
      .source_root_argument_index = source_root_argument_index,
      .memory_space = memory_space,
  };
  for (loom_target_compile_report_vec_t* vec =
           report->source_low_memory_root_summaries.head;
       vec != NULL; vec = vec->next) {
    loom_target_compile_report_source_low_memory_root_summary_t* summaries =
        (loom_target_compile_report_source_low_memory_root_summary_t*)
            loom_target_compile_report_vec_rows(vec);
    for (iree_host_size_t i = 0; i < vec->count; ++i) {
      loom_target_compile_report_source_low_memory_root_summary_t* summary =
          &summaries[i];
      if (iree_string_view_equal(summary->function_name, function_name) &&
          loom_report_memory_root_compare(summary, &key) == 0) {
        return summary;
      }
    }
  }
  return NULL;
}

static iree_status_t
loom_target_compile_report_record_source_low_memory_root_summary_row(
    loom_target_compile_report_t* report,
    const loom_target_compile_report_source_low_memory_root_summary_t* row) {
  loom_target_compile_report_source_low_memory_root_summary_t* summary =
      loom_target_compile_report_find_source_low_memory_root_summary(
          report, row->function_name, row->source_root_name,
          row->source_root_argument_index, row->memory_space);
  if (summary != NULL) {
    loom_target_compile_report_merge_source_low_memory_root_summary(summary,
                                                                    row);
    return iree_ok_status();
  } else if (iree_string_view_is_empty(row->source_root_name) &&
             row->source_root_argument_index == UINT16_MAX) {
    return iree_ok_status();
  }
  return loom_target_compile_report_row_list_append(
      &report->source_low_memory_root_summaries, sizeof(*row),
      report->allocator, row);
}

static loom_target_compile_report_source_low_memory_argument_summary_t*
loom_target_compile_report_find_source_low_memory_argument_summary(
    loom_target_compile_report_t* report, iree_string_view_t function_name,
    uint16_t source_root_argument_index, iree_string_view_t memory_space) {
  if (source_root_argument_index == UINT16_MAX) {
    return NULL;
  }
  const loom_target_compile_report_source_low_memory_argument_summary_t key = {
      .function_name = function_name,
      .source_root_argument_index = source_root_argument_index,
      .memory_space = memory_space,
  };
  for (loom_target_compile_report_vec_t* vec =
           report->source_low_memory_argument_summaries.head;
       vec != NULL; vec = vec->next) {
    loom_target_compile_report_source_low_memory_argument_summary_t* summaries =
        (loom_target_compile_report_source_low_memory_argument_summary_t*)
            loom_target_compile_report_vec_rows(vec);
    for (iree_host_size_t i = 0; i < vec->count; ++i) {
      loom_target_compile_report_source_low_memory_argument_summary_t* summary =
          &summaries[i];
      if (iree_string_view_equal(summary->function_name, function_name) &&
          loom_report_memory_argument_compare(summary, &key) == 0) {
        return summary;
      }
    }
  }
  return NULL;
}

static iree_status_t
loom_target_compile_report_record_source_low_memory_argument_summary_row(
    loom_target_compile_report_t* report,
    const loom_target_compile_report_source_low_memory_argument_summary_t*
        row) {
  loom_target_compile_report_source_low_memory_argument_summary_t* summary =
      loom_target_compile_report_find_source_low_memory_argument_summary(
          report, row->function_name, row->source_root_argument_index,
          row->memory_space);
  if (summary != NULL) {
    loom_target_compile_report_merge_source_low_memory_argument_summary(summary,
                                                                        row);
    return iree_ok_status();
  } else if (row->source_root_argument_index == UINT16_MAX) {
    return iree_ok_status();
  }
  return loom_target_compile_report_row_list_append(
      &report->source_low_memory_argument_summaries, sizeof(*row),
      report->allocator, row);
}

static loom_target_compile_report_source_low_memory_argument_packet_summary_t
loom_target_compile_report_source_low_memory_argument_packet_summary_from_row(
    const loom_target_compile_report_source_low_memory_row_t* row) {
  return (
      loom_target_compile_report_source_low_memory_argument_packet_summary_t){
      .function_name = row->function_name,
      .source_root_name = row->source_root_name,
      .source_root_argument_index = row->source_root_argument_index,
      .memory_space = row->memory_space,
      .operation_kind = row->operation_kind,
      .packet_key = row->packet_key,
      .strategy_key = row->strategy_key,
      .fallback_reason = row->fallback_reason,
      .storage_element_format = row->storage_element_format,
      .storage_scale_format = row->storage_scale_format,
      .storage_secondary_scale_format = row->storage_secondary_scale_format,
      .storage_payload_packing = row->storage_payload_packing,
      .storage_scale_topology = row->storage_scale_topology,
      .storage_affine_policy = row->storage_affine_policy,
      .storage_rounding_policy = row->storage_rounding_policy,
      .storage_codebook_policy = row->storage_codebook_policy,
      .storage_sparsity_policy = row->storage_sparsity_policy,
  };
}

static int
loom_target_compile_report_source_low_memory_argument_packet_summaries_compare(
    const void* lhs_data, const void* rhs_data) {
  const loom_target_compile_report_source_low_memory_argument_packet_summary_t*
      lhs = lhs_data;
  const loom_target_compile_report_source_low_memory_argument_packet_summary_t*
      rhs = rhs_data;
  LOOM_REPORT_MEMORY_COMPARE_SCALAR(source_root_argument_index);
  LOOM_REPORT_MEMORY_COMPARE_STRING(memory_space);
  LOOM_REPORT_MEMORY_COMPARE_STRING(operation_kind);
  LOOM_REPORT_MEMORY_COMPARE_STRING(packet_key);
  LOOM_REPORT_MEMORY_COMPARE_STRING(strategy_key);
  LOOM_REPORT_MEMORY_COMPARE_STRING(fallback_reason);
  LOOM_REPORT_MEMORY_COMPARE_STRING(storage_element_format);
  LOOM_REPORT_MEMORY_COMPARE_STRING(storage_scale_format);
  LOOM_REPORT_MEMORY_COMPARE_STRING(storage_secondary_scale_format);
  LOOM_REPORT_MEMORY_COMPARE_STRING(storage_payload_packing);
  LOOM_REPORT_MEMORY_COMPARE_STRING(storage_scale_topology);
  LOOM_REPORT_MEMORY_COMPARE_STRING(storage_affine_policy);
  LOOM_REPORT_MEMORY_COMPARE_STRING(storage_rounding_policy);
  LOOM_REPORT_MEMORY_COMPARE_STRING(storage_codebook_policy);
  LOOM_REPORT_MEMORY_COMPARE_STRING(storage_sparsity_policy);
  return 0;
}

static loom_target_compile_report_source_low_memory_argument_packet_summary_t*
loom_target_compile_report_find_source_low_memory_argument_packet_summary(
    loom_target_compile_report_t* report,
    const loom_target_compile_report_source_low_memory_argument_packet_summary_t*
        row) {
  if (row->source_root_argument_index == UINT16_MAX) {
    return NULL;
  }
  for (loom_target_compile_report_vec_t* vec =
           report->source_low_memory_argument_packet_summaries.head;
       vec != NULL; vec = vec->next) {
    loom_target_compile_report_source_low_memory_argument_packet_summary_t*
        summaries =
            (loom_target_compile_report_source_low_memory_argument_packet_summary_t*)
                loom_target_compile_report_vec_rows(vec);
    for (iree_host_size_t i = 0; i < vec->count; ++i) {
      loom_target_compile_report_source_low_memory_argument_packet_summary_t*
          summary = &summaries[i];
      if (iree_string_view_equal(summary->function_name, row->function_name) &&
          loom_target_compile_report_source_low_memory_argument_packet_summaries_compare(
              summary, row) == 0) {
        return summary;
      }
    }
  }
  return NULL;
}

static iree_status_t
loom_target_compile_report_record_source_low_memory_argument_packet_summary_row(
    loom_target_compile_report_t* report,
    const loom_target_compile_report_source_low_memory_argument_packet_summary_t*
        row) {
  loom_target_compile_report_source_low_memory_argument_packet_summary_t* summary =
      loom_target_compile_report_find_source_low_memory_argument_packet_summary(
          report, row);
  if (summary != NULL) {
    loom_target_compile_report_merge_source_low_memory_argument_packet_summary(
        summary, row);
    return iree_ok_status();
  } else if (row->source_root_argument_index == UINT16_MAX) {
    return iree_ok_status();
  }
  return loom_target_compile_report_row_list_append(
      &report->source_low_memory_argument_packet_summaries, sizeof(*row),
      report->allocator, row);
}

static loom_target_compile_report_source_low_memory_strategy_summary_t
loom_target_compile_report_source_low_memory_strategy_summary_from_row(
    const loom_target_compile_report_source_low_memory_row_t* row) {
  return (loom_target_compile_report_source_low_memory_strategy_summary_t){
      .function_name = row->function_name,
      .memory_space = row->memory_space,
      .operation_kind = row->operation_kind,
      .packet_key = row->packet_key,
      .strategy_key = row->strategy_key,
      .fallback_reason = row->fallback_reason,
      .storage_element_format = row->storage_element_format,
      .storage_scale_format = row->storage_scale_format,
      .storage_secondary_scale_format = row->storage_secondary_scale_format,
      .storage_payload_packing = row->storage_payload_packing,
      .storage_scale_topology = row->storage_scale_topology,
      .storage_affine_policy = row->storage_affine_policy,
      .storage_rounding_policy = row->storage_rounding_policy,
      .storage_codebook_policy = row->storage_codebook_policy,
      .storage_sparsity_policy = row->storage_sparsity_policy,
  };
}

static int
loom_target_compile_report_source_low_memory_strategy_summaries_compare(
    const void* lhs_data, const void* rhs_data) {
  const loom_target_compile_report_source_low_memory_strategy_summary_t* lhs =
      lhs_data;
  const loom_target_compile_report_source_low_memory_strategy_summary_t* rhs =
      rhs_data;
  LOOM_REPORT_MEMORY_COMPARE_STRING(memory_space);
  LOOM_REPORT_MEMORY_COMPARE_STRING(operation_kind);
  LOOM_REPORT_MEMORY_COMPARE_STRING(packet_key);
  LOOM_REPORT_MEMORY_COMPARE_STRING(strategy_key);
  LOOM_REPORT_MEMORY_COMPARE_STRING(fallback_reason);
  LOOM_REPORT_MEMORY_COMPARE_STRING(storage_element_format);
  LOOM_REPORT_MEMORY_COMPARE_STRING(storage_scale_format);
  LOOM_REPORT_MEMORY_COMPARE_STRING(storage_secondary_scale_format);
  LOOM_REPORT_MEMORY_COMPARE_STRING(storage_payload_packing);
  LOOM_REPORT_MEMORY_COMPARE_STRING(storage_scale_topology);
  LOOM_REPORT_MEMORY_COMPARE_STRING(storage_affine_policy);
  LOOM_REPORT_MEMORY_COMPARE_STRING(storage_rounding_policy);
  LOOM_REPORT_MEMORY_COMPARE_STRING(storage_codebook_policy);
  LOOM_REPORT_MEMORY_COMPARE_STRING(storage_sparsity_policy);
  return 0;
}

static loom_target_compile_report_source_low_memory_strategy_summary_t*
loom_target_compile_report_find_source_low_memory_strategy_summary(
    loom_target_compile_report_t* report,
    const loom_target_compile_report_source_low_memory_strategy_summary_t*
        row) {
  if (iree_string_view_is_empty(row->strategy_key)) {
    return NULL;
  }
  for (loom_target_compile_report_vec_t* vec =
           report->source_low_memory_strategy_summaries.head;
       vec != NULL; vec = vec->next) {
    loom_target_compile_report_source_low_memory_strategy_summary_t* summaries =
        (loom_target_compile_report_source_low_memory_strategy_summary_t*)
            loom_target_compile_report_vec_rows(vec);
    for (iree_host_size_t i = 0; i < vec->count; ++i) {
      loom_target_compile_report_source_low_memory_strategy_summary_t* summary =
          &summaries[i];
      if (iree_string_view_equal(summary->function_name, row->function_name) &&
          loom_target_compile_report_source_low_memory_strategy_summaries_compare(
              summary, row) == 0) {
        return summary;
      }
    }
  }
  return NULL;
}

static iree_status_t
loom_target_compile_report_record_source_low_memory_strategy_summary_row(
    loom_target_compile_report_t* report,
    const loom_target_compile_report_source_low_memory_strategy_summary_t*
        row) {
  loom_target_compile_report_source_low_memory_strategy_summary_t* summary =
      loom_target_compile_report_find_source_low_memory_strategy_summary(report,
                                                                         row);
  if (summary != NULL) {
    loom_target_compile_report_merge_source_low_memory_strategy_summary(summary,
                                                                        row);
    return iree_ok_status();
  } else if (iree_string_view_is_empty(row->strategy_key)) {
    return iree_ok_status();
  }
  return loom_target_compile_report_row_list_append(
      &report->source_low_memory_strategy_summaries, sizeof(*row),
      report->allocator, row);
}

static int loom_target_compile_report_source_low_bank_service_summaries_compare(
    const void* lhs_data, const void* rhs_data) {
  const loom_target_compile_report_source_low_bank_service_summary_t* lhs =
      lhs_data;
  const loom_target_compile_report_source_low_bank_service_summary_t* rhs =
      rhs_data;
  LOOM_REPORT_MEMORY_COMPARE_STRING(source_op_name);
  LOOM_REPORT_MEMORY_COMPARE_SCALAR(source_op_kind);
  LOOM_REPORT_MEMORY_COMPARE_STRING(source_root_name);
  LOOM_REPORT_MEMORY_COMPARE_SCALAR(source_root_argument_index);
  LOOM_REPORT_MEMORY_COMPARE_STRING(memory_space);
  LOOM_REPORT_MEMORY_COMPARE_STRING(operation_kind);
  LOOM_REPORT_MEMORY_COMPARE_STRING(packet_key);
  LOOM_REPORT_MEMORY_COMPARE_STRING(strategy_key);
  LOOM_REPORT_MEMORY_COMPARE_STRING(model_key);
  LOOM_REPORT_MEMORY_COMPARE_STRING(model_revision);
  LOOM_REPORT_MEMORY_COMPARE_STRING(model_evidence);
  LOOM_REPORT_MEMORY_COMPARE_STRING(request_policy);
  LOOM_REPORT_MEMORY_COMPARE_SCALAR(wave_size);
  LOOM_REPORT_MEMORY_COMPARE_SCALAR(bank_count);
  LOOM_REPORT_MEMORY_COMPARE_SCALAR(bank_word_byte_count);
  LOOM_REPORT_MEMORY_COMPARE_SCALAR(packet_byte_count);
  return 0;
}

static loom_target_compile_report_source_low_bank_service_summary_t*
loom_target_compile_report_find_source_low_bank_service_summary(
    loom_target_compile_report_t* report,
    const loom_target_compile_report_source_low_bank_service_summary_t* row) {
  for (loom_target_compile_report_vec_t* vec =
           report->source_low_bank_service_summaries.head;
       vec != NULL; vec = vec->next) {
    loom_target_compile_report_source_low_bank_service_summary_t* summaries =
        (loom_target_compile_report_source_low_bank_service_summary_t*)
            loom_target_compile_report_vec_rows(vec);
    for (iree_host_size_t i = 0; i < vec->count; ++i) {
      loom_target_compile_report_source_low_bank_service_summary_t* summary =
          &summaries[i];
      if (iree_string_view_equal(summary->function_name, row->function_name) &&
          loom_target_compile_report_source_low_bank_service_summaries_compare(
              summary, row) == 0) {
        return summary;
      }
    }
  }
  return NULL;
}

static void loom_target_compile_report_merge_unknown_bank_service_reason(
    loom_target_compile_report_source_low_bank_service_summary_t* target,
    iree_string_view_t source_reason, bool source_has_mixed_reasons,
    uint64_t source_unknown_packet_count) {
  if (source_unknown_packet_count == 0) {
    return;
  }
  if (target->summary.unknown_packet_count == 0 &&
      target->summary.unmodeled_packet_count == 0) {
    target->unknown_reason = source_reason;
    target->has_mixed_unknown_reasons = source_has_mixed_reasons;
    return;
  }
  if (target->has_mixed_unknown_reasons || source_has_mixed_reasons ||
      !iree_string_view_equal(target->unknown_reason, source_reason)) {
    target->unknown_reason = iree_string_view_empty();
    target->has_mixed_unknown_reasons = true;
  }
}

static void loom_target_compile_report_accumulate_bank_service_summary(
    loom_target_compile_report_bank_service_summary_t* summary,
    const loom_target_compile_report_source_low_memory_row_t* row) {
  const loom_target_compile_report_bank_service_t* bank_service =
      &row->bank_service;
  if (iree_string_view_is_empty(bank_service->proof)) {
    return;
  }
  if (iree_string_view_is_empty(bank_service->model_key)) {
    ++summary->unmodeled_packet_count;
    ++summary->unknown_dynamic_packet_count;
    return;
  }

  ++summary->modeled_packet_count;
  if (!iree_string_view_equal(bank_service->proof, IREE_SV("exact"))) {
    ++summary->unknown_packet_count;
    ++summary->unknown_dynamic_packet_count;
    return;
  }

  ++summary->exact_packet_count;
  if (iree_string_view_equal(bank_service->classification,
                             IREE_SV("conflict-free"))) {
    ++summary->conflict_free_packet_count;
  } else if (iree_string_view_equal(bank_service->classification,
                                    IREE_SV("conflicted"))) {
    ++summary->conflicted_packet_count;
  }
  summary->required_round_count += bank_service->required_rounds;
  summary->uncontended_round_count += bank_service->uncontended_rounds;
  summary->extra_round_count += bank_service->extra_rounds;
  summary->maximum_request_multiplicity =
      iree_max(summary->maximum_request_multiplicity,
               bank_service->maximum_request_multiplicity);

  if (row->execution_count_plus_one ==
      LOOM_TARGET_COMPILE_REPORT_SOURCE_LOW_MEMORY_EXECUTION_COUNT_PLUS_ONE_UNKNOWN) {
    ++summary->unknown_dynamic_packet_count;
    return;
  }

  const uint64_t execution_count = row->execution_count_plus_one - 1;
  uint64_t dynamic_required_round_count = 0;
  uint64_t dynamic_uncontended_round_count = 0;
  uint64_t dynamic_extra_round_count = 0;
  const bool dynamic_counts_ok =
      iree_checked_mul_u64(bank_service->required_rounds, execution_count,
                           &dynamic_required_round_count) &&
      iree_checked_mul_u64(bank_service->uncontended_rounds, execution_count,
                           &dynamic_uncontended_round_count) &&
      iree_checked_mul_u64(bank_service->extra_rounds, execution_count,
                           &dynamic_extra_round_count);
  uint64_t new_dynamic_packet_count = summary->dynamic_packet_count;
  uint64_t new_dynamic_required_round_count =
      summary->dynamic_required_round_count;
  uint64_t new_dynamic_uncontended_round_count =
      summary->dynamic_uncontended_round_count;
  uint64_t new_dynamic_extra_round_count = summary->dynamic_extra_round_count;
  const bool dynamic_accumulation_ok =
      dynamic_counts_ok &&
      iree_checked_add_u64(new_dynamic_packet_count, execution_count,
                           &new_dynamic_packet_count) &&
      iree_checked_add_u64(new_dynamic_required_round_count,
                           dynamic_required_round_count,
                           &new_dynamic_required_round_count) &&
      iree_checked_add_u64(new_dynamic_uncontended_round_count,
                           dynamic_uncontended_round_count,
                           &new_dynamic_uncontended_round_count) &&
      iree_checked_add_u64(new_dynamic_extra_round_count,
                           dynamic_extra_round_count,
                           &new_dynamic_extra_round_count);
  if (!dynamic_accumulation_ok) {
    ++summary->unknown_dynamic_packet_count;
    return;
  }

  ++summary->exact_dynamic_packet_count;
  summary->dynamic_packet_count = new_dynamic_packet_count;
  summary->dynamic_required_round_count = new_dynamic_required_round_count;
  summary->dynamic_uncontended_round_count =
      new_dynamic_uncontended_round_count;
  summary->dynamic_extra_round_count = new_dynamic_extra_round_count;
}

static loom_target_compile_report_source_low_bank_service_summary_t
loom_target_compile_report_source_low_bank_service_summary_from_row(
    const loom_target_compile_report_source_low_memory_row_t* row) {
  const loom_target_compile_report_bank_service_t* bank_service =
      &row->bank_service;
  return (loom_target_compile_report_source_low_bank_service_summary_t){
      .function_name = row->function_name,
      .source_op_name = row->source_op_name,
      .source_op_kind = row->source_op_kind,
      .source_root_name = row->source_root_name,
      .source_root_argument_index = row->source_root_argument_index,
      .memory_space = row->memory_space,
      .operation_kind = row->operation_kind,
      .packet_key = row->packet_key,
      .strategy_key = row->strategy_key,
      .model_key = bank_service->model_key,
      .model_revision = bank_service->model_revision,
      .model_evidence = bank_service->model_evidence,
      .request_policy = bank_service->request_policy,
      .wave_size = bank_service->wave_size,
      .bank_count = bank_service->bank_count,
      .bank_word_byte_count = bank_service->bank_word_byte_count,
      .packet_byte_count = bank_service->packet_byte_count,
  };
}

static iree_status_t
loom_target_compile_report_record_source_low_bank_service_summary_row(
    loom_target_compile_report_t* report,
    const loom_target_compile_report_source_low_bank_service_summary_t* row) {
  loom_target_compile_report_source_low_bank_service_summary_t* summary =
      loom_target_compile_report_find_source_low_bank_service_summary(report,
                                                                      row);
  if (summary != NULL) {
    loom_target_compile_report_merge_unknown_bank_service_reason(
        summary, row->unknown_reason, row->has_mixed_unknown_reasons,
        row->summary.unknown_packet_count +
            row->summary.unmodeled_packet_count);
    loom_target_compile_report_accumulate_bank_service_summaries(
        &summary->summary, &row->summary);
    return iree_ok_status();
  }
  return loom_target_compile_report_row_list_append(
      &report->source_low_bank_service_summaries, sizeof(*row),
      report->allocator, row);
}

static iree_status_t
loom_target_compile_report_record_source_low_bank_service_summary(
    loom_target_compile_report_memory_batch_t* batch,
    const loom_target_compile_report_source_low_memory_row_t* row) {
  if (iree_string_view_is_empty(row->bank_service.proof)) {
    return iree_ok_status();
  }
  const loom_target_compile_report_source_low_bank_service_summary_t key =
      loom_target_compile_report_source_low_bank_service_summary_from_row(row);
  loom_target_compile_report_source_low_bank_service_summary_t* summary = NULL;
  IREE_RETURN_IF_ERROR(loom_report_memory_group_get_or_append(
      batch, &batch->groups.bank_services,
      &batch->report->source_low_bank_service_summaries, sizeof(key), &key,
      loom_target_compile_report_source_low_bank_service_summaries_compare,
      (void**)&summary));
  loom_target_compile_report_merge_unknown_bank_service_reason(
      summary, row->bank_service.unknown_reason,
      /*source_has_mixed_reasons=*/false,
      iree_string_view_equal(row->bank_service.proof, IREE_SV("exact")) ? 0
                                                                        : 1);
  loom_target_compile_report_accumulate_bank_service_summary(&summary->summary,
                                                             row);
  return iree_ok_status();
}

static int loom_target_compile_report_subgroup_accesses_compare(
    const loom_target_compile_report_subgroup_access_t* lhs,
    const loom_target_compile_report_subgroup_access_t* rhs) {
  LOOM_REPORT_MEMORY_COMPARE_STRING(proof);
  LOOM_REPORT_MEMORY_COMPARE_STRING(lane_address_proof);
  LOOM_REPORT_MEMORY_COMPARE_STRING(active_lane_proof);
  LOOM_REPORT_MEMORY_COMPARE_STRING(lane_mapping);
  LOOM_REPORT_MEMORY_COMPARE_STRING(interval_coverage);
  LOOM_REPORT_MEMORY_COMPARE_STRING(unknown_reason);
  LOOM_REPORT_MEMORY_COMPARE_SCALAR(subgroup_size);
  LOOM_REPORT_MEMORY_COMPARE_SCALAR(active_lane_count);
  LOOM_REPORT_MEMORY_COMPARE_SCALAR(lane_term_count);
  LOOM_REPORT_MEMORY_COMPARE_SCALAR(per_lane_packet_byte_count);
  LOOM_REPORT_MEMORY_COMPARE_SCALAR(linear_lane_byte_stride);
  LOOM_REPORT_MEMORY_COMPARE_SCALAR(subgroup_requested_byte_count);
  LOOM_REPORT_MEMORY_COMPARE_SCALAR(subgroup_unique_byte_count);
  LOOM_REPORT_MEMORY_COMPARE_SCALAR(subgroup_span_byte_count);
  LOOM_REPORT_MEMORY_COMPARE_SCALAR(maximum_adjacent_lane_delta_bytes);
  LOOM_REPORT_MEMORY_COMPARE_SCALAR(maximum_uncovered_byte_gap_bytes);
  LOOM_REPORT_MEMORY_COMPARE_SCALAR(distinct_lane_address_count);
  LOOM_REPORT_MEMORY_COMPARE_SCALAR(contiguous_region_count);
  for (uint8_t i = 0; i < lhs->lane_term_count; ++i) {
    LOOM_REPORT_MEMORY_COMPARE_SCALAR(lane_terms[i].divisor);
    LOOM_REPORT_MEMORY_COMPARE_SCALAR(lane_terms[i].modulus);
    LOOM_REPORT_MEMORY_COMPARE_SCALAR(lane_terms[i].byte_stride);
  }
  return 0;
}

static int
loom_target_compile_report_source_low_subgroup_access_summaries_compare(
    const void* lhs_data, const void* rhs_data) {
  const loom_target_compile_report_source_low_subgroup_access_summary_t* lhs =
      lhs_data;
  const loom_target_compile_report_source_low_subgroup_access_summary_t* rhs =
      rhs_data;
  LOOM_REPORT_MEMORY_COMPARE_STRING(source_op_name);
  LOOM_REPORT_MEMORY_COMPARE_SCALAR(source_op_kind);
  LOOM_REPORT_MEMORY_COMPARE_STRING(source_root_name);
  LOOM_REPORT_MEMORY_COMPARE_SCALAR(source_root_argument_index);
  LOOM_REPORT_MEMORY_COMPARE_STRING(memory_space);
  LOOM_REPORT_MEMORY_COMPARE_STRING(operation_kind);
  LOOM_REPORT_MEMORY_COMPARE_STRING(packet_key);
  LOOM_REPORT_MEMORY_COMPARE_STRING(strategy_key);
  return loom_target_compile_report_subgroup_accesses_compare(&lhs->access,
                                                              &rhs->access);
}

#undef LOOM_REPORT_MEMORY_COMPARE_STRING
#undef LOOM_REPORT_MEMORY_COMPARE_SCALAR

static loom_target_compile_report_source_low_subgroup_access_summary_t*
loom_target_compile_report_find_source_low_subgroup_access_summary(
    loom_target_compile_report_t* report,
    const loom_target_compile_report_source_low_subgroup_access_summary_t*
        row) {
  for (loom_target_compile_report_vec_t* vec =
           report->source_low_subgroup_access_summaries.head;
       vec != NULL; vec = vec->next) {
    loom_target_compile_report_source_low_subgroup_access_summary_t* summaries =
        (loom_target_compile_report_source_low_subgroup_access_summary_t*)
            loom_target_compile_report_vec_rows(vec);
    for (iree_host_size_t i = 0; i < vec->count; ++i) {
      if (iree_string_view_equal(summaries[i].function_name,
                                 row->function_name) &&
          loom_target_compile_report_source_low_subgroup_access_summaries_compare(
              &summaries[i], row) == 0) {
        return &summaries[i];
      }
    }
  }
  return NULL;
}

static void loom_target_compile_report_accumulate_subgroup_access_summary(
    loom_target_compile_report_subgroup_access_summary_t* summary,
    const loom_target_compile_report_source_low_memory_row_t* row) {
  const loom_target_compile_report_subgroup_access_t* access =
      &row->subgroup_access;
  if (iree_string_view_is_empty(access->proof)) {
    return;
  }

  ++summary->modeled_packet_count;
  if (!iree_string_view_equal(access->proof, IREE_SV("exact"))) {
    ++summary->unknown_packet_count;
    ++summary->unknown_dynamic_packet_count;
    return;
  }

  ++summary->exact_packet_count;
  const bool is_dense =
      iree_string_view_equal(access->interval_coverage, IREE_SV("dense"));
  const bool is_gapped =
      iree_string_view_equal(access->interval_coverage, IREE_SV("gapped"));
  const bool is_overlapping = access->subgroup_requested_byte_count >
                              access->subgroup_unique_byte_count;
  summary->dense_packet_count += is_dense ? 1 : 0;
  summary->gapped_packet_count += is_gapped ? 1 : 0;
  summary->overlapping_packet_count += is_overlapping ? 1 : 0;

  if (row->execution_count_plus_one ==
      LOOM_TARGET_COMPILE_REPORT_SOURCE_LOW_MEMORY_EXECUTION_COUNT_PLUS_ONE_UNKNOWN) {
    ++summary->unknown_dynamic_packet_count;
    return;
  }

  const uint64_t execution_count = row->execution_count_plus_one - 1;
  uint64_t new_dynamic_packet_count = summary->dynamic_packet_count;
  uint64_t new_dynamic_dense_packet_count = summary->dynamic_dense_packet_count;
  uint64_t new_dynamic_gapped_packet_count =
      summary->dynamic_gapped_packet_count;
  uint64_t new_dynamic_overlapping_packet_count =
      summary->dynamic_overlapping_packet_count;
  const bool accumulation_ok =
      iree_checked_add_u64(new_dynamic_packet_count, execution_count,
                           &new_dynamic_packet_count) &&
      (!is_dense ||
       iree_checked_add_u64(new_dynamic_dense_packet_count, execution_count,
                            &new_dynamic_dense_packet_count)) &&
      (!is_gapped ||
       iree_checked_add_u64(new_dynamic_gapped_packet_count, execution_count,
                            &new_dynamic_gapped_packet_count)) &&
      (!is_overlapping ||
       iree_checked_add_u64(new_dynamic_overlapping_packet_count,
                            execution_count,
                            &new_dynamic_overlapping_packet_count));
  if (!accumulation_ok) {
    ++summary->unknown_dynamic_packet_count;
    return;
  }

  ++summary->exact_dynamic_packet_count;
  summary->dynamic_packet_count = new_dynamic_packet_count;
  summary->dynamic_dense_packet_count = new_dynamic_dense_packet_count;
  summary->dynamic_gapped_packet_count = new_dynamic_gapped_packet_count;
  summary->dynamic_overlapping_packet_count =
      new_dynamic_overlapping_packet_count;
}

static loom_target_compile_report_source_low_subgroup_access_summary_t
loom_target_compile_report_source_low_subgroup_access_summary_from_row(
    const loom_target_compile_report_source_low_memory_row_t* row) {
  return (loom_target_compile_report_source_low_subgroup_access_summary_t){
      .function_name = row->function_name,
      .source_op_name = row->source_op_name,
      .source_op_kind = row->source_op_kind,
      .source_root_name = row->source_root_name,
      .source_root_argument_index = row->source_root_argument_index,
      .memory_space = row->memory_space,
      .operation_kind = row->operation_kind,
      .packet_key = row->packet_key,
      .strategy_key = row->strategy_key,
      .access = row->subgroup_access,
  };
}

static iree_status_t
loom_target_compile_report_record_source_low_subgroup_access_summary_row(
    loom_target_compile_report_t* report,
    const loom_target_compile_report_source_low_subgroup_access_summary_t*
        row) {
  loom_target_compile_report_source_low_subgroup_access_summary_t* summary =
      loom_target_compile_report_find_source_low_subgroup_access_summary(report,
                                                                         row);
  if (summary != NULL) {
    loom_target_compile_report_accumulate_subgroup_access_summaries(
        &summary->summary, &row->summary);
    return iree_ok_status();
  }
  return loom_target_compile_report_row_list_append(
      &report->source_low_subgroup_access_summaries, sizeof(*row),
      report->allocator, row);
}

static iree_status_t
loom_target_compile_report_record_source_low_subgroup_access_summary(
    loom_target_compile_report_memory_batch_t* batch,
    const loom_target_compile_report_source_low_memory_row_t* row) {
  if (iree_string_view_is_empty(row->subgroup_access.proof)) {
    return iree_ok_status();
  }
  const loom_target_compile_report_source_low_subgroup_access_summary_t key =
      loom_target_compile_report_source_low_subgroup_access_summary_from_row(
          row);
  loom_target_compile_report_source_low_subgroup_access_summary_t* summary =
      NULL;
  IREE_RETURN_IF_ERROR(loom_report_memory_group_get_or_append(
      batch, &batch->groups.subgroup_accesses,
      &batch->report->source_low_subgroup_access_summaries, sizeof(key), &key,
      loom_target_compile_report_source_low_subgroup_access_summaries_compare,
      (void**)&summary));
  loom_target_compile_report_accumulate_subgroup_access_summary(
      &summary->summary, row);
  return iree_ok_status();
}

iree_status_t loom_target_compile_report_merge_source_low_memory_details(
    loom_target_compile_report_t* target,
    const loom_target_compile_report_t* source) {
  IREE_RETURN_IF_ERROR(loom_target_compile_report_row_list_append_all(
      &target->source_low_memory_rows, &source->source_low_memory_rows,
      sizeof(loom_target_compile_report_source_low_memory_row_t),
      target->allocator));
  for (const loom_target_compile_report_vec_t* vec =
           source->source_low_memory_root_summaries.head;
       vec != NULL; vec = vec->next) {
    const loom_target_compile_report_source_low_memory_root_summary_t* rows =
        (const loom_target_compile_report_source_low_memory_root_summary_t*)
            loom_target_compile_report_vec_const_rows(vec);
    for (iree_host_size_t i = 0; i < vec->count; ++i) {
      IREE_RETURN_IF_ERROR(
          loom_target_compile_report_record_source_low_memory_root_summary_row(
              target, &rows[i]));
    }
  }
  for (const loom_target_compile_report_vec_t* vec =
           source->source_low_memory_argument_summaries.head;
       vec != NULL; vec = vec->next) {
    const loom_target_compile_report_source_low_memory_argument_summary_t* rows =
        (const loom_target_compile_report_source_low_memory_argument_summary_t*)
            loom_target_compile_report_vec_const_rows(vec);
    for (iree_host_size_t i = 0; i < vec->count; ++i) {
      IREE_RETURN_IF_ERROR(
          loom_target_compile_report_record_source_low_memory_argument_summary_row(
              target, &rows[i]));
    }
  }
  for (const loom_target_compile_report_vec_t* vec =
           source->source_low_memory_argument_packet_summaries.head;
       vec != NULL; vec = vec->next) {
    const loom_target_compile_report_source_low_memory_argument_packet_summary_t*
        rows =
            (const loom_target_compile_report_source_low_memory_argument_packet_summary_t*)
                loom_target_compile_report_vec_const_rows(vec);
    for (iree_host_size_t i = 0; i < vec->count; ++i) {
      IREE_RETURN_IF_ERROR(
          loom_target_compile_report_record_source_low_memory_argument_packet_summary_row(
              target, &rows[i]));
    }
  }
  for (const loom_target_compile_report_vec_t* vec =
           source->source_low_memory_strategy_summaries.head;
       vec != NULL; vec = vec->next) {
    const loom_target_compile_report_source_low_memory_strategy_summary_t* rows =
        (const loom_target_compile_report_source_low_memory_strategy_summary_t*)
            loom_target_compile_report_vec_const_rows(vec);
    for (iree_host_size_t i = 0; i < vec->count; ++i) {
      IREE_RETURN_IF_ERROR(
          loom_target_compile_report_record_source_low_memory_strategy_summary_row(
              target, &rows[i]));
    }
  }
  for (const loom_target_compile_report_vec_t* vec =
           source->source_low_bank_service_summaries.head;
       vec != NULL; vec = vec->next) {
    const loom_target_compile_report_source_low_bank_service_summary_t* rows =
        (const loom_target_compile_report_source_low_bank_service_summary_t*)
            loom_target_compile_report_vec_const_rows(vec);
    for (iree_host_size_t i = 0; i < vec->count; ++i) {
      IREE_RETURN_IF_ERROR(
          loom_target_compile_report_record_source_low_bank_service_summary_row(
              target, &rows[i]));
    }
  }
  for (const loom_target_compile_report_vec_t* vec =
           source->source_low_subgroup_access_summaries.head;
       vec != NULL; vec = vec->next) {
    const loom_target_compile_report_source_low_subgroup_access_summary_t* rows =
        (const loom_target_compile_report_source_low_subgroup_access_summary_t*)
            loom_target_compile_report_vec_const_rows(vec);
    for (iree_host_size_t i = 0; i < vec->count; ++i) {
      IREE_RETURN_IF_ERROR(
          loom_target_compile_report_record_source_low_subgroup_access_summary_row(
              target, &rows[i]));
    }
  }
  return iree_ok_status();
}

static bool loom_target_compile_report_source_low_memory_row_is_load(
    const loom_target_compile_report_source_low_memory_row_t* row) {
  return iree_string_view_equal(row->operation_kind, IREE_SV("load"));
}

static bool loom_target_compile_report_source_low_memory_row_is_store(
    const loom_target_compile_report_source_low_memory_row_t* row) {
  return iree_string_view_equal(row->operation_kind, IREE_SV("store"));
}

static void loom_target_compile_report_accumulate_source_low_memory_summary(
    loom_target_compile_report_source_low_memory_summary_t* summary,
    const loom_target_compile_report_source_low_memory_row_t* row) {
  const uint64_t lane_count = row->vector_lane_count;
  const uint64_t source_byte_count = lane_count * row->element_byte_count;
  const bool is_load =
      loom_target_compile_report_source_low_memory_row_is_load(row);
  const bool is_store =
      loom_target_compile_report_source_low_memory_row_is_store(row);
  ++summary->packet_count;
  summary->source_lane_count += lane_count;
  summary->source_byte_count += source_byte_count;
  if (is_load) {
    ++summary->load_packet_count;
    summary->read_byte_count += source_byte_count;
    loom_target_compile_report_accumulate_memory_interval_summary(
        &summary->read_interval_envelope, &row->source_interval);
  } else if (is_store) {
    ++summary->store_packet_count;
    summary->write_byte_count += source_byte_count;
    loom_target_compile_report_accumulate_memory_interval_summary(
        &summary->write_interval_envelope, &row->source_interval);
  }
  summary->issued_read_byte_count += row->issued_read_byte_count;
  summary->issued_write_byte_count += row->issued_write_byte_count;
  summary->issued_read_unknown_width_count +=
      row->issued_read_unknown_width_count;
  summary->issued_write_unknown_width_count +=
      row->issued_write_unknown_width_count;
  loom_target_compile_report_accumulate_memory_interval_summary(
      &summary->interval_envelope, &row->source_interval);
  if (row->execution_count_plus_one ==
      LOOM_TARGET_COMPILE_REPORT_SOURCE_LOW_MEMORY_EXECUTION_COUNT_PLUS_ONE_UNKNOWN) {
    ++summary->unknown_dynamic_packet_count;
  } else {
    const uint64_t execution_count = row->execution_count_plus_one - 1;
    uint64_t dynamic_source_byte_count = 0;
    uint64_t dynamic_issued_read_byte_count = 0;
    uint64_t dynamic_issued_write_byte_count = 0;
    uint64_t dynamic_issued_read_unknown_width_count = 0;
    uint64_t dynamic_issued_write_unknown_width_count = 0;
    const bool dynamic_counts_ok =
        iree_checked_mul_u64(source_byte_count, execution_count,
                             &dynamic_source_byte_count) &&
        iree_checked_mul_u64(row->issued_read_byte_count, execution_count,
                             &dynamic_issued_read_byte_count) &&
        iree_checked_mul_u64(row->issued_write_byte_count, execution_count,
                             &dynamic_issued_write_byte_count) &&
        iree_checked_mul_u64(row->issued_read_unknown_width_count,
                             execution_count,
                             &dynamic_issued_read_unknown_width_count) &&
        iree_checked_mul_u64(row->issued_write_unknown_width_count,
                             execution_count,
                             &dynamic_issued_write_unknown_width_count);
    uint64_t new_dynamic_packet_count = summary->dynamic_packet_count;
    uint64_t new_dynamic_source_byte_count = summary->dynamic_source_byte_count;
    uint64_t new_dynamic_read_byte_count = summary->dynamic_read_byte_count;
    uint64_t new_dynamic_write_byte_count = summary->dynamic_write_byte_count;
    uint64_t new_dynamic_issued_read_byte_count =
        summary->dynamic_issued_read_byte_count;
    uint64_t new_dynamic_issued_write_byte_count =
        summary->dynamic_issued_write_byte_count;
    uint64_t new_dynamic_issued_read_unknown_width_count =
        summary->dynamic_issued_read_unknown_width_count;
    uint64_t new_dynamic_issued_write_unknown_width_count =
        summary->dynamic_issued_write_unknown_width_count;
    bool dynamic_accumulation_ok =
        dynamic_counts_ok &&
        iree_checked_add_u64(new_dynamic_packet_count, execution_count,
                             &new_dynamic_packet_count) &&
        iree_checked_add_u64(new_dynamic_source_byte_count,
                             dynamic_source_byte_count,
                             &new_dynamic_source_byte_count) &&
        iree_checked_add_u64(new_dynamic_issued_read_byte_count,
                             dynamic_issued_read_byte_count,
                             &new_dynamic_issued_read_byte_count) &&
        iree_checked_add_u64(new_dynamic_issued_write_byte_count,
                             dynamic_issued_write_byte_count,
                             &new_dynamic_issued_write_byte_count) &&
        iree_checked_add_u64(new_dynamic_issued_read_unknown_width_count,
                             dynamic_issued_read_unknown_width_count,
                             &new_dynamic_issued_read_unknown_width_count) &&
        iree_checked_add_u64(new_dynamic_issued_write_unknown_width_count,
                             dynamic_issued_write_unknown_width_count,
                             &new_dynamic_issued_write_unknown_width_count);
    if (dynamic_accumulation_ok && is_load) {
      dynamic_accumulation_ok = iree_checked_add_u64(
          new_dynamic_read_byte_count, dynamic_source_byte_count,
          &new_dynamic_read_byte_count);
    } else if (dynamic_accumulation_ok && is_store) {
      dynamic_accumulation_ok = iree_checked_add_u64(
          new_dynamic_write_byte_count, dynamic_source_byte_count,
          &new_dynamic_write_byte_count);
    }
    if (!dynamic_accumulation_ok) {
      ++summary->unknown_dynamic_packet_count;
    } else {
      ++summary->exact_dynamic_packet_count;
      summary->dynamic_packet_count = new_dynamic_packet_count;
      summary->dynamic_source_byte_count = new_dynamic_source_byte_count;
      summary->dynamic_read_byte_count = new_dynamic_read_byte_count;
      summary->dynamic_write_byte_count = new_dynamic_write_byte_count;
      summary->dynamic_issued_read_byte_count =
          new_dynamic_issued_read_byte_count;
      summary->dynamic_issued_write_byte_count =
          new_dynamic_issued_write_byte_count;
      summary->dynamic_issued_read_unknown_width_count =
          new_dynamic_issued_read_unknown_width_count;
      summary->dynamic_issued_write_unknown_width_count =
          new_dynamic_issued_write_unknown_width_count;
    }
  }
  if (lane_count == 1) {
    ++summary->scalar_packet_count;
  } else if (lane_count > 1) {
    ++summary->vector_packet_count;
    if (row->element_byte_count == 0 || row->vector_lane_stride_bytes == 0) {
      ++summary->unknown_stride_vector_packet_count;
    } else if (row->vector_lane_stride_bytes == row->element_byte_count) {
      ++summary->contiguous_vector_packet_count;
    } else {
      ++summary->strided_vector_packet_count;
    }
  }
}

static iree_status_t
loom_target_compile_report_record_source_low_memory_root_summary(
    loom_target_compile_report_memory_batch_t* batch,
    const loom_target_compile_report_source_low_memory_row_t* row,
    loom_target_compile_report_source_low_memory_root_summary_t** out_summary) {
  *out_summary = NULL;
  if (iree_string_view_is_empty(row->source_root_name) &&
      row->source_root_argument_index == UINT16_MAX) {
    return iree_ok_status();
  }
  const loom_target_compile_report_source_low_memory_root_summary_t key = {
      .function_name = row->function_name,
      .source_root_name = row->source_root_name,
      .source_root_argument_index = row->source_root_argument_index,
      .memory_space = row->memory_space,
  };
  IREE_RETURN_IF_ERROR(loom_report_memory_group_get_or_append(
      batch, &batch->groups.roots,
      &batch->report->source_low_memory_root_summaries, sizeof(key), &key,
      loom_report_memory_root_compare, (void**)out_summary));
  loom_target_compile_report_accumulate_source_low_memory_summary(
      &(*out_summary)->summary, row);
  return iree_ok_status();
}

static iree_status_t
loom_target_compile_report_record_source_low_memory_argument_summary(
    loom_target_compile_report_memory_batch_t* batch,
    const loom_target_compile_report_source_low_memory_row_t* row,
    loom_target_compile_report_source_low_memory_argument_summary_t**
        out_summary) {
  *out_summary = NULL;
  if (row->source_root_argument_index == UINT16_MAX) {
    return iree_ok_status();
  }
  const loom_target_compile_report_source_low_memory_argument_summary_t key = {
      .function_name = row->function_name,
      .source_root_name = row->source_root_name,
      .source_root_argument_index = row->source_root_argument_index,
      .memory_space = row->memory_space,
  };
  IREE_RETURN_IF_ERROR(loom_report_memory_group_get_or_append(
      batch, &batch->groups.arguments,
      &batch->report->source_low_memory_argument_summaries, sizeof(key), &key,
      loom_report_memory_argument_compare, (void**)out_summary));
  loom_target_compile_report_accumulate_source_low_memory_summary(
      &(*out_summary)->summary, row);
  return iree_ok_status();
}

static iree_status_t
loom_target_compile_report_record_source_low_memory_strategy_summary(
    loom_target_compile_report_memory_batch_t* batch,
    const loom_target_compile_report_source_low_memory_row_t* row) {
  if (iree_string_view_is_empty(row->strategy_key)) {
    return iree_ok_status();
  }
  const loom_target_compile_report_source_low_memory_strategy_summary_t key =
      loom_target_compile_report_source_low_memory_strategy_summary_from_row(
          row);
  loom_target_compile_report_source_low_memory_strategy_summary_t* summary =
      NULL;
  IREE_RETURN_IF_ERROR(loom_report_memory_group_get_or_append(
      batch, &batch->groups.strategies,
      &batch->report->source_low_memory_strategy_summaries, sizeof(key), &key,
      loom_target_compile_report_source_low_memory_strategy_summaries_compare,
      (void**)&summary));
  loom_target_compile_report_accumulate_source_low_memory_summary(
      &summary->summary, row);
  return iree_ok_status();
}

iree_status_t loom_target_compile_report_memory_batch_append(
    loom_target_compile_report_memory_batch_t* batch,
    const loom_target_compile_report_source_low_memory_row_t* row) {
  loom_target_compile_report_t* report = batch->report;
  report->detail_flags |= LOOM_TARGET_COMPILE_REPORT_DETAIL_SOURCE_LOW_ROWS;
  IREE_RETURN_IF_ERROR(loom_target_compile_report_row_list_append(
      &report->source_low_memory_rows, sizeof(*row), report->allocator, row));
  loom_target_compile_report_accumulate_source_low_memory_summary(
      &report->source_low_memory_summary, row);
  loom_target_compile_report_accumulate_bank_service_summary(
      &report->bank_service_summary, row);
  loom_target_compile_report_accumulate_subgroup_access_summary(
      &report->subgroup_access_summary, row);
  if (iree_allocator_is_null(report->allocator)) {
    return iree_ok_status();
  }
  loom_target_compile_report_source_low_memory_root_summary_t* root = NULL;
  IREE_RETURN_IF_ERROR(
      loom_target_compile_report_record_source_low_memory_root_summary(
          batch, row, &root));
  loom_target_compile_report_source_low_memory_argument_summary_t* argument =
      NULL;
  IREE_RETURN_IF_ERROR(
      loom_target_compile_report_record_source_low_memory_argument_summary(
          batch, row, &argument));
  if (row->source_root_argument_index != UINT16_MAX) {
    loom_target_compile_report_source_low_memory_argument_packet_summary_t key =
        loom_target_compile_report_source_low_memory_argument_packet_summary_from_row(
            row);
    loom_target_compile_report_source_low_memory_argument_packet_summary_t*
        summary = NULL;
    IREE_RETURN_IF_ERROR(loom_report_memory_group_get_or_append(
        batch, &batch->groups.argument_packets,
        &report->source_low_memory_argument_packet_summaries, sizeof(key), &key,
        loom_target_compile_report_source_low_memory_argument_packet_summaries_compare,
        (void**)&summary));
    loom_target_compile_report_accumulate_source_low_memory_summary(
        &key.summary, row);
    loom_target_compile_report_merge_source_low_memory_argument_packet_summary(
        summary, &key);
  }
  IREE_RETURN_IF_ERROR(
      loom_target_compile_report_record_source_low_memory_strategy_summary(
          batch, row));
  IREE_RETURN_IF_ERROR(
      loom_target_compile_report_record_source_low_bank_service_summary(batch,
                                                                        row));
  IREE_RETURN_IF_ERROR(
      loom_target_compile_report_record_source_low_subgroup_access_summary(
          batch, row));
  if (root == NULL) {
    return iree_ok_status();
  }
  loom_target_compile_report_memory_interval_unique_kind_t kind =
      LOOM_TARGET_COMPILE_REPORT_MEMORY_INTERVAL_UNIQUE_NONE;
  if (loom_target_compile_report_memory_interval_is_exact_static(
          &row->source_interval)) {
    kind = LOOM_TARGET_COMPILE_REPORT_MEMORY_INTERVAL_UNIQUE_STATIC;
  } else if (loom_target_compile_report_memory_interval_has_exact_symbolic(
                 &row->source_interval)) {
    kind = LOOM_TARGET_COMPILE_REPORT_MEMORY_INTERVAL_UNIQUE_SYMBOLIC;
  } else {
    return iree_ok_status();
  }
  loom_target_compile_report_vec_t* tail = report->source_low_memory_rows.tail;
  const loom_target_compile_report_source_low_memory_row_t* retained_row =
      (const loom_target_compile_report_source_low_memory_row_t*)
          loom_target_compile_report_vec_const_rows(tail) +
      tail->count - 1;
  const loom_report_memory_entry_t entry = {
      .row = retained_row,
      .root = root,
      .argument = argument,
      .ordinal = batch->entries.count,
      .kind = kind,
      .direction =
          loom_target_compile_report_source_low_memory_row_is_load(row)
              ? LOOM_REPORT_MEMORY_READ
          : loom_target_compile_report_source_low_memory_row_is_store(row)
              ? LOOM_REPORT_MEMORY_WRITE
              : LOOM_REPORT_MEMORY_ALL,
  };
  IREE_RETURN_IF_ERROR(loom_target_compile_report_row_list_append(
      &batch->entries, sizeof(entry), iree_arena_allocator(batch->arena),
      &entry));
  return iree_ok_status();
}
