// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_IR_INTERN_TABLE_H_
#define LOOM_IR_INTERN_TABLE_H_

#include "loom/util/segmented_storage.h"

#ifdef __cplusplus
extern "C" {
#endif

// Bucket chunk width covering the full canonical index domain at 0.75 load.
#define LOOM_INTERN_SEGMENT_CAPACITY 128u
#define LOOM_INTERN_SEGMENT_SHIFT 7u
#define LOOM_INTERN_SEGMENT_MASK (LOOM_INTERN_SEGMENT_CAPACITY - 1u)

static_assert((1u << LOOM_INTERN_SEGMENT_SHIFT) == LOOM_INTERN_SEGMENT_CAPACITY,
              "intern segment capacity must match its index shift");
static_assert((uint64_t)LOOM_INTERN_SEGMENT_CAPACITY *
                      LOOM_SEGMENTED_STORAGE_MAX_SEGMENT_COUNT >=
                  (UINT64_C(1) << 33),
              "intern storage must cover 32-bit indices at 0.75 load");

// One occupied bucket or an empty bucket identified by UINT32_MAX.
typedef struct loom_intern_bucket_t {
  // Canonical row index, or UINT32_MAX when the bucket is empty.
  uint32_t index;
  // Retained structural hash, meaningful only for an occupied bucket.
  uint32_t hash;
} loom_intern_bucket_t;

// Coallocated hashes and indices with one allocation and lifetime.
typedef struct loom_intern_segment_t {
  // Initialized buckets, including any unused tail of a small logical table.
  loom_intern_bucket_t buckets[LOOM_INTERN_SEGMENT_CAPACITY];
} loom_intern_segment_t;

// Forward-linear-probing index of module-owned canonical rows. The caller's
// arena owns all segments; growth reuses old buckets rather than retaining
// obsolete generations. Canonical rows are owned separately and never moved
// by this table. Bucket contents and probe slots can change during growth.
typedef struct loom_intern_table_t {
  // Number of occupied buckets.
  iree_host_size_t count;
  // Logical power-of-two bucket count, or zero before lazy allocation.
  iree_host_size_t capacity;
  // Stable bucket chunks and their initialized pointer directory.
  loom_segmented_storage_t segments;
} loom_intern_table_t;

// Compares a candidate with the canonical row at the supplied index.
typedef bool (*loom_intern_equal_fn_t)(const void* context, uint32_t index);

typedef struct loom_intern_probe_t {
  // Existing canonical row index, or UINT32_MAX for an absent candidate.
  uint32_t index;
  // Matching or vacant slot, valid until the table grows or is mutated.
  iree_host_size_t slot;
} loom_intern_probe_t;

// Returns a power-of-two bucket capacity for the requested entry capacity at
// the maximum 0.75 load factor. The entry capacity is at least three, ensuring
// the resulting table has at least four buckets and an integral load threshold.
iree_host_size_t loom_intern_table_capacity_for_entries(
    iree_host_size_t entry_capacity);

// Initializes an empty table with a power-of-two capacity of at least four.
// A zero capacity keeps allocation lazy. Allocation failure leaves an empty
// initialized table and restores the arena's prior allocation state.
iree_status_t loom_intern_table_initialize(iree_arena_allocator_t* arena,
                                           iree_host_size_t capacity,
                                           loom_intern_table_t* out_table);

// Clears occupied buckets while retaining their storage and logical capacity.
void loom_intern_table_clear(loom_intern_table_t* table);

// Allocates initial capacity or grows to a larger power-of-two capacity. The
// vacant seam is an empty old slot retained by a failed probe, ignored for an
// empty table. Failure preserves the table and the prior arena allocation
// state; successful growth invalidates previously retained probe slots.
iree_status_t loom_intern_table_grow(iree_arena_allocator_t* arena,
                                     iree_host_size_t capacity,
                                     iree_host_size_t vacant_seam,
                                     loom_intern_table_t* table);

// Returns a bucket at an initialized logical slot.
static inline const loom_intern_bucket_t* loom_intern_table_const_bucket(
    const loom_intern_table_t* table, iree_host_size_t slot) {
  const loom_intern_segment_t* segment =
      (const loom_intern_segment_t*)loom_segmented_storage_const_segment(
          &table->segments, (uint32_t)(slot >> LOOM_INTERN_SEGMENT_SHIFT));
  return &segment->buckets[slot & LOOM_INTERN_SEGMENT_MASK];
}

// Returns a mutable bucket for the table's exclusive producer.
static inline loom_intern_bucket_t* loom_intern_table_bucket(
    loom_intern_table_t* table, iree_host_size_t slot) {
  loom_intern_segment_t* segment =
      (loom_intern_segment_t*)loom_segmented_storage_segment(
          &table->segments, (uint32_t)(slot >> LOOM_INTERN_SEGMENT_SHIFT));
  return &segment->buckets[slot & LOOM_INTERN_SEGMENT_MASK];
}

// Tests whether insertion preserves the maximum load factor without growth.
static inline bool loom_intern_table_has_insert_capacity(
    const loom_intern_table_t* table) {
  return table->count * 4 < table->capacity * 3;
}

// Finds a vacant slot for a known-unique value in a non-full table.
static inline iree_host_size_t loom_intern_table_find_empty_slot(
    const loom_intern_table_t* table, uint32_t hash) {
  const iree_host_size_t mask = table->capacity - 1;
  iree_host_size_t slot = hash & mask;
  while (loom_intern_table_const_bucket(table, slot)->index != UINT32_MAX) {
    slot = (slot + 1) & mask;
  }
  return slot;
}

// Probes a candidate once, retaining the vacant slot on a miss.
static inline loom_intern_probe_t loom_intern_table_probe(
    const loom_intern_table_t* table, uint32_t hash,
    loom_intern_equal_fn_t equal_fn, const void* equal_context) {
  if (table->capacity == 0) {
    return (loom_intern_probe_t){/* .index = */ UINT32_MAX, /* .slot = */ 0};
  }
  const iree_host_size_t mask = table->capacity - 1;
  iree_host_size_t slot = hash & mask;
  while (true) {
    const loom_intern_bucket_t* bucket =
        loom_intern_table_const_bucket(table, slot);
    if (bucket->index == UINT32_MAX) {
      return (loom_intern_probe_t){/* .index = */ UINT32_MAX,
                                   /* .slot = */ slot};
    }
    if (bucket->hash == hash && equal_fn(equal_context, bucket->index)) {
      return (loom_intern_probe_t){/* .index = */ bucket->index,
                                   /* .slot = */ slot};
    }
    slot = (slot + 1) & mask;
  }
}

// Reserves a positive |insertion_count| after a miss, updating the first key's
// known vacant slot only when growth changes placement. Remaining keys find
// their slots after preceding insertions. No mutation may intervene between
// probe and reserve. Failure preserves the table and prior arena allocation
// state.
static inline iree_status_t loom_intern_table_reserve_insert(
    iree_arena_allocator_t* arena, loom_intern_table_t* table, uint32_t hash,
    iree_host_size_t insertion_count, iree_host_size_t* inout_slot) {
  const iree_host_size_t required_count = table->count + insertion_count;
  if (required_count * 4 <= table->capacity * 3) {
    return iree_ok_status();
  }
  iree_host_size_t capacity = table->capacity == 0 ? 32 : table->capacity * 2;
  if (required_count * 4 > capacity * 3) {
    capacity = loom_intern_table_capacity_for_entries(required_count);
  }
  IREE_RETURN_IF_ERROR(
      loom_intern_table_grow(arena, capacity, *inout_slot, table));
  *inout_slot = loom_intern_table_find_empty_slot(table, hash);
  return iree_ok_status();
}

// Publishes a known-unique row into its reserved vacant slot.
static inline void loom_intern_table_insert(loom_intern_table_t* table,
                                            iree_host_size_t slot,
                                            uint32_t hash, uint32_t index) {
  *loom_intern_table_bucket(table, slot) =
      (loom_intern_bucket_t){/* .index = */ index, /* .hash = */ hash};
  ++table->count;
}

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_IR_INTERN_TABLE_H_
