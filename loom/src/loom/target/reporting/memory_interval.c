// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/target/reporting/memory_interval.h"

#include <string.h>

#include "loom/target/reporting/row_list.h"
#include "loom/util/adaptive_sort.h"
#include "loom/util/segmented_storage.h"

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

iree_status_t loom_target_compile_report_memory_intervals_finish(
    loom_target_compile_report_row_list_t* packets,
    iree_arena_allocator_t* arena) {
  if (packets->count == 0) {
    return iree_ok_status();
  }
  loom_segmented_storage_t storage;
  loom_segmented_storage_initialize(
      LOOM_REPORT_MEMORY_WORD_BLOCK_CAPACITY *
          sizeof(loom_report_memory_entry_ptr_t),
      iree_alignof(loom_report_memory_entry_ptr_t), &storage);
  IREE_RETURN_IF_ERROR(loom_report_memory_allocate_segments(
      &storage,
      iree_host_size_ceil_div(packets->count,
                              LOOM_REPORT_MEMORY_WORD_BLOCK_CAPACITY),
      arena));
  const loom_report_memory_entries_t entries = {.storage = &storage};
  iree_host_size_t count = 0;
  for (loom_target_compile_report_vec_t* block = packets->head; block != NULL;
       block = block->next) {
    loom_report_memory_entry_t* rows =
        (loom_report_memory_entry_t*)loom_target_compile_report_vec_rows(block);
    for (iree_host_size_t i = 0; i < block->count; ++i) {
      rows[i].sort_key = (uintptr_t)rows[i].root;
      *loom_report_memory_entry_at(entries, count++) = &rows[i];
    }
  }
  loom_report_memory_sort_entries(NULL, entries, count);
  iree_arena_allocator_t group_arena;
  iree_arena_initialize(arena->block_pool, &group_arena);
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

void loom_target_compile_report_accumulate_memory_interval_summary(
    loom_target_compile_report_memory_interval_summary_t* target,
    const loom_target_compile_report_memory_interval_t* interval) {
  if (!loom_target_compile_report_memory_interval_has_envelope(interval)) {
    return;
  }
  loom_target_compile_report_merge_memory_interval_envelope_bounds(
      target, interval->begin_min_bytes, interval->end_max_bytes);
  ++target->packet_count;
}

void loom_target_compile_report_merge_memory_interval_envelope_summary(
    loom_target_compile_report_memory_interval_summary_t* target,
    const loom_target_compile_report_memory_interval_summary_t* source) {
  if (source->packet_count == 0) {
    return;
  }
  loom_target_compile_report_merge_memory_interval_envelope_bounds(
      target, source->envelope_begin_min_bytes, source->envelope_end_max_bytes);
  target->packet_count += source->packet_count;
}

void loom_target_compile_report_forget_memory_interval_unique_accounting(
    loom_target_compile_report_memory_interval_summary_t* summary) {
  summary->exact_static_packet_count = 0;
  summary->exact_symbolic_packet_count = 0;
  summary->unique_byte_count = 0;
}

iree_status_t loom_target_compile_report_memory_intervals_append(
    loom_target_compile_report_row_list_t* entries,
    const loom_target_compile_report_source_low_memory_row_t* row,
    loom_target_compile_report_source_low_memory_root_summary_t* root,
    loom_target_compile_report_source_low_memory_argument_summary_t* argument,
    iree_arena_allocator_t* arena) {
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
  const loom_report_memory_entry_t entry = {
      .row = row,
      .root = root,
      .argument = argument,
      .ordinal = entries->count,
      .kind = kind,
      .direction =
          iree_string_view_equal(row->operation_kind, IREE_SV("load"))
              ? LOOM_REPORT_MEMORY_READ
          : iree_string_view_equal(row->operation_kind, IREE_SV("store"))
              ? LOOM_REPORT_MEMORY_WRITE
              : LOOM_REPORT_MEMORY_ALL,
  };
  return loom_target_compile_report_row_list_append(
      entries, sizeof(entry), iree_arena_allocator(arena), &entry);
}
