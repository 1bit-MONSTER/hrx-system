// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Value-type subrange of a registered memory region.
//
// An iree_async_span_t identifies a contiguous byte range within an
// iree_async_region_t. Spans are non-owning values (like iree_string_view_t).
// Accepted operations acquire their own references to registered regions.
//
// Operations that transfer data (recv, send, read, write) take spans to
// describe their buffers. The proactor uses the span's region to access
// backend-specific handles for zero-copy I/O.
//
// ## Region lifetime during I/O
//
// When an operation containing a span is accepted by a proactor, common
// operation ownership retains the span's region through the final completion
// callback. The caller may release its own region reference after submit
// returns OK. Explicit buffer unregistration remains illegal while an
// operation is in flight; wait for final completion before unregistering.
// Retaining a region does not take ownership of arbitrary host memory wrapped
// by register_buffer; that backing allocation must also remain valid.
//
// For spans with region == NULL (unregistered memory), no retain/release
// occurs. The caller must ensure the memory remains valid through the final
// completion callback.

#ifndef IREE_ASYNC_SPAN_H_
#define IREE_ASYNC_SPAN_H_

#include "iree/async/region.h"
#include "iree/base/api.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

//===----------------------------------------------------------------------===//
// Span
//===----------------------------------------------------------------------===//

// A non-owning reference to a contiguous byte range.
//
// When region is non-NULL, the span references a subrange of a registered
// memory region. The offset is relative to the registered range, independently
// of its CPU mapping or native device address. Providers lower that offset
// using the region's backend handles for zero-copy I/O.
//
// When region is NULL, the span references unregistered memory (offset holds
// the raw pointer cast to iree_host_size_t). The proactor falls back to
// copy-based I/O. The caller must ensure the memory remains valid for the
// span's lifetime.
typedef struct iree_async_span_t {
  // The region this span references. NULL for unregistered (raw pointer) spans.
  iree_async_region_t* region;

  // When region is non-NULL: byte offset into the registered range.
  // When region is NULL: the raw pointer cast to iree_host_size_t.
  iree_host_size_t offset;

  // Byte length of the span.
  iree_host_size_t length;
} iree_async_span_t;

// Creates a span referencing a subrange of a region.
static inline iree_async_span_t iree_async_span_make(
    iree_async_region_t* region, iree_host_size_t offset,
    iree_host_size_t length) {
  iree_async_span_t span;
  span.region = region;
  span.offset = offset;
  span.length = length;
  return span;
}

// Creates a span covering an entire region.
static inline iree_async_span_t iree_async_span_from_region(
    iree_async_region_t* region, iree_host_size_t region_length) {
  iree_async_span_t span;
  span.region = region;
  span.offset = 0;
  span.length = region_length;
  return span;
}

// Creates a span from a raw (unregistered) pointer.
// The caller must ensure the memory remains valid for the span's lifetime.
// Proactors use copy-based I/O for raw pointer spans (no zero-copy).
static inline iree_async_span_t iree_async_span_from_ptr(
    void* ptr, iree_host_size_t length) {
  iree_async_span_t span;
  span.region = NULL;
  span.offset = (iree_host_size_t)(uintptr_t)ptr;
  span.length = length;
  return span;
}

// Returns an empty span.
static inline iree_async_span_t iree_async_span_empty(void) {
  iree_async_span_t span = {0};
  return span;
}

// Returns true if the span has zero length.
static inline bool iree_async_span_is_empty(iree_async_span_t span) {
  return span.length == 0;
}

// Returns true if the span references CPU-accessible memory.
// For raw pointer spans (region == NULL), returns true (caller provided ptr).
// For region spans, returns true if region->base_ptr is non-NULL.
//
// CPU-inaccessible spans require a provider accepting their native memory
// handles. Host copying or content inspection requires CPU accessibility.
static inline bool iree_async_span_is_cpu_accessible(iree_async_span_t span) {
  // Raw pointer spans are always CPU-accessible (caller gave us a pointer).
  if (!span.region) {
    return true;
  }
  // Region spans are CPU-accessible if base_ptr is non-NULL.
  return span.region->base_ptr != NULL;
}

// Returns the host pointer for the start of the span.
// Requires iree_async_span_is_cpu_accessible(span).
// For registered spans, returns region->base_ptr + offset.
// For raw pointer spans (region == NULL), returns the pointer stored in offset.
static inline uint8_t* iree_async_span_ptr(iree_async_span_t span) {
  if (!span.region) {
    return (uint8_t*)(uintptr_t)span.offset;
  }
  return (uint8_t*)span.region->base_ptr + span.offset;
}

// Returns the span's memory as an iree_byte_span_t (mutable).
// Requires iree_async_span_is_cpu_accessible(span).
static inline iree_byte_span_t iree_async_span_data(iree_async_span_t span) {
  return iree_make_byte_span(iree_async_span_ptr(span), span.length);
}

// Returns the span's memory as an iree_const_byte_span_t (read-only).
// Requires iree_async_span_is_cpu_accessible(span).
static inline iree_const_byte_span_t iree_async_span_const_data(
    iree_async_span_t span) {
  return iree_make_const_byte_span(iree_async_span_ptr(span), span.length);
}

//===----------------------------------------------------------------------===//
// Span list
//===----------------------------------------------------------------------===//

// A scatter-gather list of spans for vectored I/O operations.
typedef struct iree_async_span_list_t {
  iree_async_span_t* values;
  iree_host_size_t count;
} iree_async_span_list_t;

// Creates a span list from a pointer to an array of spans and a count.
// The |values| array must remain valid for the span list's lifetime.
static inline iree_async_span_list_t iree_async_span_list_make(
    iree_async_span_t* values, iree_host_size_t count) {
  iree_async_span_list_t list;
  list.values = values;
  list.count = count;
  return list;
}

// Returns an empty span list with zero entries.
static inline iree_async_span_list_t iree_async_span_list_empty(void) {
  iree_async_span_list_t list = {0};
  return list;
}

// Returns true if the span list contains no entries.
static inline bool iree_async_span_list_is_empty(iree_async_span_list_t list) {
  return list.count == 0;
}

//===----------------------------------------------------------------------===//
// Region lifetime helpers
//===----------------------------------------------------------------------===//

// Retains the registered region referenced by |span|, if any.
static inline void iree_async_span_retain_region(iree_async_span_t span) {
  if (span.region) {
    iree_async_region_retain(span.region);
  }
}

// Releases the registered region referenced by |span|, if any.
static inline void iree_async_span_release_region(iree_async_span_t span) {
  iree_async_region_release(span.region);
}

// Retains every registered region referenced by |list|.
static inline void iree_async_span_list_retain_regions(
    iree_async_span_list_t list) {
  for (iree_host_size_t i = 0; i < list.count; ++i) {
    iree_async_span_retain_region(list.values[i]);
  }
}

// Releases every registered region referenced by |list|.
static inline void iree_async_span_list_release_regions(
    iree_async_span_list_t list) {
  for (iree_host_size_t i = 0; i < list.count; ++i) {
    iree_async_span_release_region(list.values[i]);
  }
}

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // IREE_ASYNC_SPAN_H_
