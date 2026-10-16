// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Stable arena-backed storage for fixed-size segments.
//
// Segments are appended and never moved. Typed owners choose the payload
// layout within each segment and map their logical indexes to segment indexes.
// The directory keeps a small number of segment pointers inline, expands once
// to a fixed primary page, and allocates further pages through a bounded
// directory of page groups. Payload storage is never copied during growth.

#ifndef LOOM_UTIL_SEGMENTED_STORAGE_H_
#define LOOM_UTIL_SEGMENTED_STORAGE_H_

#include "iree/base/api.h"
#include "iree/base/internal/arena.h"

#ifdef __cplusplus
extern "C" {
#endif

// Number of segment pointers stored directly in the container.
#define LOOM_SEGMENTED_STORAGE_INLINE_SEGMENT_COUNT 16u

// Number of segment pointers stored in each allocated pointer page.
#define LOOM_SEGMENTED_STORAGE_SEGMENTS_PER_PAGE 512u

// Shift mapping a segment index to its pointer-page index.
#define LOOM_SEGMENTED_STORAGE_PAGE_SHIFT 9u

// Mask mapping a segment index to its position within a pointer page.
#define LOOM_SEGMENTED_STORAGE_PAGE_MASK \
  (LOOM_SEGMENTED_STORAGE_SEGMENTS_PER_PAGE - 1u)

// Maximum segment count covered by three fixed-width directory indexes.
#define LOOM_SEGMENTED_STORAGE_MAX_SEGMENT_COUNT \
  (1u << (3 * LOOM_SEGMENTED_STORAGE_PAGE_SHIFT))

static_assert((1u << LOOM_SEGMENTED_STORAGE_PAGE_SHIFT) ==
                  LOOM_SEGMENTED_STORAGE_SEGMENTS_PER_PAGE,
              "segment pointer page size must match its index shift");

// Segment pointers whose initialized prefix is determined by segment_count.
typedef struct loom_segmented_storage_page_t {
  // Stable payload addresses; unpublished slots are uninitialized.
  void* segments[LOOM_SEGMENTED_STORAGE_SEGMENTS_PER_PAGE];
} loom_segmented_storage_page_t;

// Fixed-capacity group of segment pointer pages.
typedef struct loom_segmented_storage_page_group_t {
  // Pages covering the initialized segment prefix in this group.
  loom_segmented_storage_page_t*
      pages[LOOM_SEGMENTED_STORAGE_SEGMENTS_PER_PAGE];
} loom_segmented_storage_page_group_t;

// Top-level directory retaining the complete segment index domain.
typedef struct loom_segmented_storage_directory_t {
  // Groups covering the initialized segment prefix.
  loom_segmented_storage_page_group_t*
      groups[LOOM_SEGMENTED_STORAGE_SEGMENTS_PER_PAGE];
} loom_segmented_storage_directory_t;

// Arena-backed stable segment directory.
//
// The storage object does not own or reset its arena. Every pointer becomes
// invalid when the owner resets that arena, after which the owner must
// reinitialize the storage object before reuse. Initialized storage may be
// copied as a read-only prefix snapshot. An exclusive owner may stage appends
// through one copy, then publish that copy or discard the extension and restore
// its arena checkpoint. Appends never overwrite pointers in the original
// prefix, and sequential boundaries recreate unpublished pages without reading
// their slots. Only one copy may append at a time; snapshots do not roll back
// mutations to existing segment payloads.
typedef struct loom_segmented_storage_t {
  // Number of initialized segment pointers.
  uint32_t segment_count;
  // Size in bytes of every segment payload.
  iree_host_size_t segment_size;
  // Minimum power-of-two alignment of every segment payload.
  iree_host_size_t segment_alignment;
  // Allocated segment pointer page covering the first page of segment indexes,
  // or NULL until the inline pointer directory is exceeded.
  loom_segmented_storage_page_t* primary_page;
  // Lazily allocated page-group directory, or NULL while one page suffices.
  loom_segmented_storage_directory_t* page_directory;
  // Stable inline prefix, retained after the allocated directory grows.
  void* inline_segments[LOOM_SEGMENTED_STORAGE_INLINE_SEGMENT_COUNT];
} loom_segmented_storage_t;

// Initializes empty storage for fixed-size, fixed-alignment segments.
// No allocation is performed until the first segment is appended.
void loom_segmented_storage_initialize(iree_host_size_t segment_size,
                                       iree_host_size_t segment_alignment,
                                       loom_segmented_storage_t* out_storage);

// Moves initialized |source| storage into uninitialized |out_storage| without
// moving segment payloads. |source| is left uninitialized and must be
// initialized again before reuse.
void loom_segmented_storage_move(loom_segmented_storage_t* source,
                                 loom_segmented_storage_t* out_storage);

// Appends one uninitialized segment allocated from |arena| and returns it.
// Existing segment payload pointers remain stable.
// Allocation failure leaves the directory and published count unchanged.
// Successful appends initialize only pointer slots beyond the prior prefix.
// To discard a staged extension, retain the prior header and restore the arena
// checkpoint preceding the extension before appending through that header
// again. New payloads must not have been published to another owner.
iree_status_t loom_segmented_storage_append(loom_segmented_storage_t* storage,
                                            iree_arena_allocator_t* arena,
                                            void** out_segment);

// Returns a mutable segment payload by its zero-based index.
static inline void* loom_segmented_storage_segment(
    loom_segmented_storage_t* storage, uint32_t segment_index) {
  IREE_ASSERT(segment_index < storage->segment_count);
  if (IREE_LIKELY(segment_index <
                  LOOM_SEGMENTED_STORAGE_INLINE_SEGMENT_COUNT)) {
    return storage->inline_segments[segment_index];
  }
  const uint32_t page_index =
      segment_index >> LOOM_SEGMENTED_STORAGE_PAGE_SHIFT;
  loom_segmented_storage_page_t* page =
      page_index == 0
          ? storage->primary_page
          : storage->page_directory
                ->groups[page_index >> LOOM_SEGMENTED_STORAGE_PAGE_SHIFT]
                ->pages[page_index & LOOM_SEGMENTED_STORAGE_PAGE_MASK];
  return page->segments[segment_index & LOOM_SEGMENTED_STORAGE_PAGE_MASK];
}

// Returns a const segment payload by its zero-based index.
static inline const void* loom_segmented_storage_const_segment(
    const loom_segmented_storage_t* storage, uint32_t segment_index) {
  IREE_ASSERT(segment_index < storage->segment_count);
  if (IREE_LIKELY(segment_index <
                  LOOM_SEGMENTED_STORAGE_INLINE_SEGMENT_COUNT)) {
    return storage->inline_segments[segment_index];
  }
  const uint32_t page_index =
      segment_index >> LOOM_SEGMENTED_STORAGE_PAGE_SHIFT;
  const loom_segmented_storage_page_t* page =
      page_index == 0
          ? storage->primary_page
          : storage->page_directory
                ->groups[page_index >> LOOM_SEGMENTED_STORAGE_PAGE_SHIFT]
                ->pages[page_index & LOOM_SEGMENTED_STORAGE_PAGE_MASK];
  return page->segments[segment_index & LOOM_SEGMENTED_STORAGE_PAGE_MASK];
}

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_UTIL_SEGMENTED_STORAGE_H_
