// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_IR_STRING_TABLE_H_
#define LOOM_IR_STRING_TABLE_H_

#include "loom/ir/types.h"
#include "loom/util/segmented_storage.h"

#ifdef __cplusplus
extern "C" {
#endif

// Number of interned string views in each stable segment.
#define LOOM_STRING_SEGMENT_CAPACITY 128u

// Shift mapping a string ID to its segment index.
#define LOOM_STRING_SEGMENT_SHIFT 7u

// Mask mapping a string ID to its row within a segment.
#define LOOM_STRING_SEGMENT_MASK (LOOM_STRING_SEGMENT_CAPACITY - 1u)

static_assert((1u << LOOM_STRING_SEGMENT_SHIFT) == LOOM_STRING_SEGMENT_CAPACITY,
              "string segment capacity must match its index shift");
static_assert((uint64_t)LOOM_STRING_SEGMENT_CAPACITY *
                      LOOM_SEGMENTED_STORAGE_MAX_SEGMENT_COUNT >=
                  (uint64_t)LOOM_STRING_ID_INVALID,
              "string storage must cover the full string ID domain");

// Stable views into individually contiguous bytes whose lifetime is guaranteed
// by the enclosing table producer.
typedef struct loom_string_segment_t {
  // Immutable views indexed by the low bits of a canonical string ID.
  iree_string_view_t entries[LOOM_STRING_SEGMENT_CAPACITY];
} loom_string_segment_t;

// Append-only string-view storage owned by an enclosing arena-backed producer.
// Table IDs and rows remain stable for that owner's lifetime; viewed bytes must
// have at least the same lifetime. Only the initialized prefix named by count
// may be read; unused segment rows are uninitialized. Owners may use a separate
// content interner to map spellings to these dense table-local IDs.
typedef struct loom_string_table_t {
  // Number of published string views.
  iree_host_size_t count;
  // Lazily allocated, stable view segments and their pointer directory.
  loom_segmented_storage_t segments;
} loom_string_table_t;

// Returns the number of allocated rows, including the unpublished tail.
static inline iree_host_size_t loom_string_table_capacity(
    const loom_string_table_t* table) {
  return (iree_host_size_t)table->segments.segment_count *
         LOOM_STRING_SEGMENT_CAPACITY;
}

// Returns a published string view with the enclosing owner's byte lifetime.
static inline iree_string_view_t loom_string_table_get(
    const loom_string_table_t* table, loom_string_id_t string_id) {
  IREE_ASSERT((iree_host_size_t)string_id < table->count);
  const loom_string_segment_t* segment =
      (const loom_string_segment_t*)loom_segmented_storage_const_segment(
          &table->segments, string_id >> LOOM_STRING_SEGMENT_SHIFT);
  return segment->entries[string_id & LOOM_STRING_SEGMENT_MASK];
}

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_IR_STRING_TABLE_H_
