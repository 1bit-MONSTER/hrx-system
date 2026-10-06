// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/ir/intern_table.h"

#include <string.h>

iree_host_size_t loom_intern_table_capacity_for_entries(
    iree_host_size_t entry_capacity) {
  return iree_host_size_next_power_of_two((entry_capacity * 4 + 2) / 3);
}

// Extends storage before moving any old bucket. The vacant seam terminates an
// old probe cluster. Walking immediately after it visits each old entry before
// reinsertion can reach its slot: a new home is the old home plus a multiple
// of old_capacity, and clearing the current slot bounds its forward probe.
iree_status_t loom_intern_table_grow(iree_arena_allocator_t* arena,
                                     iree_host_size_t capacity,
                                     iree_host_size_t vacant_seam,
                                     loom_intern_table_t* table) {
  const iree_arena_checkpoint_t checkpoint = iree_arena_checkpoint_save(arena);
  loom_intern_table_t grown = *table;
  const iree_host_size_t segment_count =
      (capacity + LOOM_INTERN_SEGMENT_MASK) >> LOOM_INTERN_SEGMENT_SHIFT;
  iree_status_t status = iree_ok_status();
  while (grown.segments.segment_count < segment_count &&
         iree_status_is_ok(status)) {
    void* segment = NULL;
    status = loom_segmented_storage_append(&grown.segments, arena, &segment);
    if (iree_status_is_ok(status)) {
      memset(segment, 0xFF, sizeof(loom_intern_segment_t));
    }
  }
  if (iree_status_is_ok(status)) {
    grown.capacity = capacity;
    if (table->capacity != 0) {
      const iree_host_size_t old_mask = table->capacity - 1;
      iree_host_size_t slot = (vacant_seam + 1) & old_mask;
      for (iree_host_size_t i = 0; i < table->capacity; ++i) {
        loom_intern_bucket_t* bucket = loom_intern_table_bucket(&grown, slot);
        const loom_intern_bucket_t entry = *bucket;
        if (entry.index != UINT32_MAX) {
          bucket->index = UINT32_MAX;
          const iree_host_size_t destination =
              loom_intern_table_find_empty_slot(&grown, entry.hash);
          *loom_intern_table_bucket(&grown, destination) = entry;
        }
        slot = (slot + 1) & old_mask;
      }
    }
    *table = grown;
  } else {
    iree_arena_checkpoint_restore(&checkpoint);
  }
  return status;
}

iree_status_t loom_intern_table_initialize(iree_arena_allocator_t* arena,
                                           iree_host_size_t capacity,
                                           loom_intern_table_t* out_table) {
  out_table->count = 0;
  out_table->capacity = 0;
  loom_segmented_storage_initialize(sizeof(loom_intern_segment_t),
                                    iree_alignof(loom_intern_segment_t),
                                    &out_table->segments);
  if (capacity == 0) {
    return iree_ok_status();
  }
  return loom_intern_table_grow(arena, capacity, /*vacant_seam=*/0, out_table);
}

void loom_intern_table_clear(loom_intern_table_t* table) {
  for (uint32_t i = 0; i < table->segments.segment_count; ++i) {
    memset(loom_segmented_storage_segment(&table->segments, i), 0xFF,
           sizeof(loom_intern_segment_t));
  }
  table->count = 0;
}
