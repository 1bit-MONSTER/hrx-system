// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef IREE_HAL_MEMORY_RESERVATION_H_
#define IREE_HAL_MEMORY_RESERVATION_H_

#include "iree/hal/memory_scope.h"
#include "iree/hal/queue.h"
#include "iree/hal/semaphore.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

typedef struct iree_hal_queue_t iree_hal_queue_t;
typedef struct iree_hal_memory_reservation_t iree_hal_memory_reservation_t;

// Geometry of an address reservation, independent of physical storage pools.
// Zero alignment/granularity selects the backend's minimum. The achieved
// granularity is queried after creation and applies to every mapping range.
// A larger granularity may reduce native mapping work (for example, the number
// of independently replaceable section views on Windows).
typedef struct iree_hal_memory_reservation_options_t {
  // Nonzero length of the stable virtual buffer, in bytes.
  iree_device_size_t byte_length;
  // Minimum power-of-two alignment of the virtual buffer's first byte.
  iree_device_size_t min_alignment;
  // Minimum power-of-two independently replaceable mapping unit, in bytes.
  iree_device_size_t mapping_granularity;
} iree_hal_memory_reservation_options_t;

// Reserves addresses for the simultaneous access described by |scope|. No
// physical payload is allocated. The sealed |group| outlives the reservation
// and all borrowed views. The length must be a multiple of the achieved native
// mapping granularity. Unsupported scopes return UNAVAILABLE; invalid byte
// geometry returns INVALID_ARGUMENT or OUT_OF_RANGE.
// On failure |out_reservation| is NULL and no ownership is transferred.
IREE_API_EXPORT iree_status_t iree_hal_memory_reservation_create(
    iree_hal_device_group_t* group, iree_hal_pool_scope_t scope,
    const iree_hal_memory_reservation_options_t* options,
    iree_allocator_t host_allocator,
    iree_hal_memory_reservation_t** out_reservation);

// Returns the borrowed stable buffer view. It may be retained in ordinary
// commands/submissions, but retaining this wrapper does not keep explicitly
// destroyed addresses or caller-owned mapped backing alive. Access to an
// unmapped range is invalid. Physical placement and visibility queries apply
// to the actual source buffers; an address reservation owns no physical pages.
IREE_API_EXPORT iree_hal_buffer_t* iree_hal_memory_reservation_buffer(
    iree_hal_memory_reservation_t* reservation);

// Returns the exact independently replaceable mapping unit, in bytes.
IREE_API_EXPORT iree_device_size_t
iree_hal_memory_reservation_mapping_granularity(
    const iree_hal_memory_reservation_t* reservation);

// Returns the canonical native projection for an admitted scope. Equal
// projection IDs mean the sites share page tables: changing either affects
// both. ANY is not an individual projection. IDs belong to this reservation.
IREE_API_EXPORT iree_status_t iree_hal_memory_reservation_resolve_projection(
    const iree_hal_memory_reservation_t* reservation,
    iree_hal_memory_scope_t scope, uint32_t* out_projection);

// Removes all native mappings and consumes the reservation owner, including
// on failure. NULL is a no-op. The caller must first complete every mapping and
// execution using these addresses. Retained buffer views only preserve wrapper
// metadata and must not be used after this call. Success certifies native
// removal; failure does not certify that formerly mapped storage is reusable.
// There is no installed-source ownership or active-use tracking.
IREE_API_EXPORT iree_status_t
iree_hal_memory_reservation_destroy(iree_hal_memory_reservation_t* reservation);

// One change to a virtual range. Mapping borrows physical storage; it does not
// copy contents, transfer ownership, or retain backing after queue completion.
// Target offsets and lengths satisfy the reservation's mapping granularity.
// Native source offsets and lengths satisfy the source's independently prepared
// granularity, which may be smaller than the target's replacement unit.
typedef struct iree_hal_memory_mapping_t {
  // Borrowed target address owner.
  iree_hal_memory_reservation_t* reservation;
  // Exact admitted site, or ANY in the reservation's domain for all
  // projections.
  iree_hal_memory_scope_t scope;
  // Destination interval, relative to the reservation's first byte.
  iree_hal_buffer_range_t target_range;
  // Ordinary buffer from a pool prepared with the MAPPING_SOURCE interface for
  // each selected site. NULL detaches the target range.
  iree_hal_buffer_t* source;
  // Byte offset within the logical source buffer; zero when detaching.
  iree_device_size_t source_offset;
  // Required native access permissions; NONE when detaching.
  iree_hal_memory_access_t access;
} iree_hal_memory_mapping_t;

//===----------------------------------------------------------------------===//
// Reservation implementation contract
//===----------------------------------------------------------------------===//

// Resolved source identity and target coordinates for one native projection.
// Public ranges and access have been validated and transient sources committed
// before native update. Arrays are sorted by target offset and nonoverlapping.
typedef struct iree_hal_memory_mapping_update_t {
  // Exact admitted target site, preserved across native projection aliases.
  uint32_t scope_id;
  // Target interval in reservation coordinates.
  iree_hal_buffer_range_t target_range;
  // Prepared native source; a NULL descriptor detaches the interval.
  iree_hal_buffer_native_binding_t source;
  // Exact requested access permissions.
  iree_hal_memory_access_t access;
} iree_hal_memory_mapping_update_t;

typedef struct iree_hal_memory_reservation_vtable_t {
  // Checked terminal native removal; consumes native state on every outcome.
  iree_status_t(IREE_API_PTR* close)(
      iree_hal_memory_reservation_t* reservation);
  // Frees metadata after explicit close and final buffer wrapper release.
  void(IREE_API_PTR* destroy)(iree_hal_memory_reservation_t* reservation);
  // Completes one native projection vector synchronously. The implementation
  // owns sticky failure state after native mutation; preparation failures must
  // not poison an unchanged reservation.
  iree_status_t(IREE_API_PTR* update)(
      iree_hal_memory_reservation_t* reservation, uint32_t projection,
      iree_host_size_t update_count,
      const iree_hal_memory_mapping_update_t* updates);
} iree_hal_memory_reservation_vtable_t;

struct iree_hal_memory_reservation_t {
  // Stable ordinary execution view and metadata reference count.
  iree_hal_buffer_t buffer;
  // Backend operations; never used for ordinary dispatch binding.
  const iree_hal_memory_reservation_vtable_t* vtable;
  // Allocator used for metadata and captured queue-map batches.
  iree_allocator_t host_allocator;
  // Achieved mapping unit shared by all native projections.
  iree_device_size_t mapping_granularity;
  // Number of distinct native page-table projections.
  uint32_t projection_count;
  // Scope-indexed native projections; INVALID for excluded sites and ANY.
  const uint32_t* projections;
  // Explicit native destruction has consumed the address owner.
  bool closed;
};

// Initializes the common buffer view and retains its immutable contract.
// The backend supplies bindings/projections and owns their metadata until its
// destroy callback. That callback releases metadata only; close owns all
// fallible native removal. The creator sets projection_count and projections.
IREE_API_EXPORT void iree_hal_memory_reservation_initialize(
    iree_hal_memory_contract_t* contract,
    const iree_hal_buffer_native_binding_t* bindings,
    iree_hal_memory_type_t memory_type, iree_device_size_t byte_length,
    iree_device_size_t mapping_granularity,
    const iree_hal_memory_reservation_vtable_t* vtable,
    iree_allocator_t host_allocator,
    iree_hal_memory_reservation_t* reservation);

// Validates and captures one complete batch for synchronous native publication.
// The returned call owns one resource reference, released by its recipient.
// Its callback runs once after queue prerequisites, returns a terminal status,
// and does not publish signals itself. Cancellation only releases the resource.
// Queue implementations choose their existing cold execution owner and retain
// operation/frontier ownership until the callback returns.
IREE_API_EXPORT iree_status_t iree_hal_memory_reservation_prepare_map(
    iree_hal_queue_t* queue, iree_host_size_t mapping_count,
    const iree_hal_memory_mapping_t* mappings, iree_hal_queue_map_flags_t flags,
    iree_hal_host_call_t* out_call);

// Captures one batch and executes it through the queue's existing host-call
// owner. Used by backends with synchronous native mapping publication. Native
// queue implementations may provide queue.map to retain GPU-ordered updates.
IREE_API_EXPORT iree_status_t iree_hal_memory_reservation_queue_map(
    iree_hal_queue_t* queue, iree_hal_semaphore_list_t waits,
    iree_hal_semaphore_list_t signals, iree_host_size_t mapping_count,
    const iree_hal_memory_mapping_t* mappings,
    iree_hal_queue_map_flags_t flags);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // IREE_HAL_MEMORY_RESERVATION_H_
