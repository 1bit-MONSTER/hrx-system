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

// Explicit mapping state supplied by the caller. These operations do not cause
// HAL to discover or track installed sources. Backends can use native bind and
// protection primitives when their APIs cannot replace arbitrary intervals.
typedef enum iree_hal_memory_mapping_operation_e {
  // Replaces any previous mapping in the range, including holes. A NULL source
  // clears the range. This is the default for callers managing sparse ranges.
  IREE_HAL_MEMORY_MAPPING_OPERATION_REPLACE = 0,
  // Installs a non-NULL source in a range the caller knows is unbound.
  IREE_HAL_MEMORY_MAPPING_OPERATION_BIND = 1,
  // Removes a complete bound range. Requires a NULL source and zero access.
  IREE_HAL_MEMORY_MAPPING_OPERATION_UNBIND = 2,
  // Changes access to a bound range. The caller supplies the same source and
  // source offset currently bound there, including when revoking all access.
  IREE_HAL_MEMORY_MAPPING_OPERATION_PROTECT = 3,
} iree_hal_memory_mapping_operation_t;

// Bit set indexed by iree_hal_memory_mapping_operation_t.
typedef uint32_t iree_hal_memory_mapping_operations_t;
#define IREE_HAL_MEMORY_MAPPING_OPERATIONS_ALL UINT32_C(0xF)

// Native address geometry for a complete simultaneous scope. A zero
// max_byte_length means no backend supports that scope. These limits describe
// addresses only; backing pools and their prepared mapping sources may impose
// additional alignment and size requirements. Address availability is checked
// at creation and is not guaranteed by a successful capability query.
typedef struct iree_hal_memory_reservation_capabilities_t {
  // Largest supported address interval, in bytes; zero when unsupported.
  iree_device_size_t max_byte_length;
  // Required multiple of the reservation length and minimum base alignment.
  iree_device_size_t reservation_granularity;
  // Minimum independently replaceable destination offset and length unit.
  iree_device_size_t mapping_granularity;
  // Largest accepted power-of-two minimum base alignment.
  iree_device_size_t max_alignment;
  // Operations supported by every projection of the requested scope.
  iree_hal_memory_mapping_operations_t operations;
} iree_hal_memory_reservation_capabilities_t;

// Queries geometry before acquiring addresses or physical backing. The same
// complete-scope qualification is used by creation. Unsupported combinations
// return OK with zeroed capabilities; invalid scope, native query, and metadata
// allocation failures propagate. The allocator is used only for temporary
// construction metadata. This is a cold planning operation.
IREE_API_EXPORT iree_status_t iree_hal_memory_reservation_query_capabilities(
    iree_hal_device_group_t* group, iree_hal_pool_scope_t scope,
    iree_allocator_t host_allocator,
    iree_hal_memory_reservation_capabilities_t* out_capabilities);

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
// Finite pools can subdivide this view using its virtual alignment and captured
// group progress services, including before mappings are installed. Their
// retained ranges and allocations remain virtual capacity; the caller orders
// every access after mapping completion and destroys child pools before the
// address owner. Mapping replacement does not change child buffer addresses.
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
// Binding and protection changes require their declared prior state. HAL does
// not validate that state or retain the source after completion. Backing with
// NONE access may be left detached natively; later protection changes supply
// the source again so the backend can install it without retained bookkeeping.
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
  // Required native access permissions; NONE revokes access. A non-NULL source
  // with NONE access remains a logical binding owned entirely by the caller.
  iree_hal_memory_access_t access;
  // Prior-state contract and requested update; zero selects general
  // replacement.
  iree_hal_memory_mapping_operation_t operation;
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
  // Caller-declared binding state, already validated against backend support.
  iree_hal_memory_mapping_operation_t operation;
} iree_hal_memory_mapping_update_t;

typedef struct iree_hal_memory_reservation_vtable_t {
  // Checked terminal native removal; consumes native state on every outcome.
  iree_status_t(IREE_API_PTR* close)(
      iree_hal_memory_reservation_t* reservation);
  // Frees metadata after explicit close and final buffer wrapper release.
  void(IREE_API_PTR* destroy)(iree_hal_memory_reservation_t* reservation);
  // Completes one native projection vector synchronously. The implementation
  // owns sticky failure state after native mutation; preparation failures must
  // not poison an unchanged reservation. NULL when updates require a native
  // queue implementation; the synchronous adapter rejects that route.
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
  // Supported operations, immutable after creation.
  iree_hal_memory_mapping_operations_t operations;
  // Native descriptor schema accepted by this address owner.
  iree_hal_memory_backend_type_t source_type;
  // Scope-indexed native projections; INVALID for excluded sites and ANY.
  const uint32_t* projections;
  // Explicit native destruction has consumed the address owner.
  bool closed;
};

// Initializes the common buffer view and retains its immutable contract.
// The backend supplies bindings/projections/backing facts and owns their
// metadata until its destroy callback. Backing facts describe virtual address
// alignment and group progress services, with no physical allocation or
// byte-touching advice. That callback releases metadata only; close owns all
// fallible native removal. The creator sets projection_count and projections.
IREE_API_EXPORT void iree_hal_memory_reservation_initialize(
    iree_hal_memory_contract_t* contract,
    const iree_hal_buffer_native_binding_t* bindings,
    const iree_hal_buffer_backing_facts_t* backing,
    iree_hal_memory_type_t memory_type, iree_device_size_t byte_length,
    iree_device_size_t mapping_granularity,
    iree_hal_memory_mapping_operations_t operations,
    iree_hal_memory_backend_type_t source_type,
    const iree_hal_memory_reservation_vtable_t* vtable,
    iree_allocator_t host_allocator,
    iree_hal_memory_reservation_t* reservation);

// Identifies the common reservation wrapper before inspecting its backend.
// The caller first resolves any ordinary subspan to its allocated buffer.
IREE_API_EXPORT bool iree_hal_memory_reservation_buffer_isa(
    iree_hal_buffer_t* buffer);

// One normalized, nonoverlapping vector targeting a native projection. Entries
// are established by capture; their source descriptors are filled by resolve.
typedef struct iree_hal_memory_mapping_batch_entry_t {
  // Explicit address owner, retained through its buffer when requested.
  iree_hal_memory_reservation_t* reservation;
  // Exact native page-table projection within the address owner.
  uint32_t projection;
  // Number of updates in this projection's contiguous vector.
  iree_host_size_t update_count;
  // Validated coordinates and permissions, with sources resolved before use.
  const iree_hal_memory_mapping_update_t* updates;
} iree_hal_memory_mapping_batch_entry_t;

// Queue-owned capture of one mapping transaction. Native queue adapters consume
// these trusted normalized entries instead of repeating public validation or
// discovering projection groups during issue. One allocation owns all metadata.
// Retaining this packet keeps optional buffer wrappers alive, not their
// explicit address owners or installed backing after completion.
typedef struct iree_hal_memory_mapping_batch_t {
  // Submission resource; release after native completion or cancellation.
  iree_hal_resource_t resource;
  // Number of distinct reservation/projection entries.
  iree_host_size_t entry_count;
  // Captured entries, ordered by address owner, projection and target offset.
  const iree_hal_memory_mapping_batch_entry_t* entries;
  // Total updates across all entries, for native payload sizing.
  iree_host_size_t update_count;
} iree_hal_memory_mapping_batch_t;

// Validates and captures a mapping transaction without resolving native
// sources. The caller owns one resource reference on success. An empty
// transaction returns NULL and can be handled as an ordinary queue barrier.
// Creation failure transfers no ownership. Buffer retention follows |flags|.
IREE_API_EXPORT iree_status_t iree_hal_memory_mapping_batch_create(
    iree_hal_queue_t* queue, iree_host_size_t mapping_count,
    const iree_hal_memory_mapping_t* mappings, iree_hal_queue_map_flags_t flags,
    iree_hal_memory_mapping_batch_t** out_batch);

// Resolves every prepared source after prerequisite allocations have committed,
// before any native mutation. Success fills the entry vectors' source bindings;
// failure leaves all destinations unchanged. The native queue retains the
// packet until completion and orders publication after its execution
// prerequisites.
IREE_API_EXPORT iree_status_t
iree_hal_memory_mapping_batch_resolve(iree_hal_memory_mapping_batch_t* batch);

// Captures one complete batch for synchronous native publication. Rejects
// reservations that require a native queue adapter before any mutation.
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
