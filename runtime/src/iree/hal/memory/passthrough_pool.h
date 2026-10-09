// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef IREE_HAL_MEMORY_PASSTHROUGH_POOL_H_
#define IREE_HAL_MEMORY_PASSTHROUGH_POOL_H_

#include "iree/base/api.h"
#include "iree/hal/memory/maintenance.h"
#include "iree/hal/memory/slab_provider.h"
#include "iree/hal/pool.h"
#include "iree/hal/slab_pool.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

typedef struct iree_async_notification_t iree_async_notification_t;

// Options for creating a pass-through HAL pool.
typedef struct iree_hal_passthrough_pool_options_t {
  // Qualified immutable memory contract retained by the pool, or NULL when
  // individual reservation parameters describe access.
  iree_hal_memory_contract_t* memory_contract;

  // Optional native completion probe inherited by child allocators. The
  // borrowed context remains alive with the pool's sealed device group.
  iree_hal_pool_epoch_query_t epoch_query;

  // ASAN policy used to shape hidden backing ranges for reservations.
  iree_hal_asan_pool_options_t asan;

  // Whether new provider-owned slabs may be acquired.
  iree_hal_slab_pool_owned_mode_t owned_mode;

  // Exact external storage classes admitted for native import. Arrays are
  // borrowed for this call and copied by the pool.
  struct {
    // Number of admitted classes.
    iree_host_size_t class_count;
    // Constructor-borrowed class descriptors.
    const iree_hal_pool_import_class_t* classes;
  } imports;

  // Optional named-memory trace identifier for logical reservations returned by
  // this pool. Empty uses a generic process-stable identifier.
  iree_string_view_t trace_name;
} iree_hal_passthrough_pool_options_t;

// Creates a pass-through pool that delegates every reservation directly to the
// slab provider. Each acquired reservation owns a new slab. After the token and
// views return, its exact release frontier is joined before native release on
// the captured maintenance owner. A failed prerequisite quarantines the slab
// until caller-quiescent pool destruction and is reported by later acquisition.
// With an empty release frontier, prior uses are already complete and sanitizer
// marking finishes before release returns. With pending history, marking is
// deferred with native retirement. Native freeing is always asynchronous.
// With DISALLOW_GROWTH, acquisition returns EXHAUSTED/GROWTH_REQUIRED without
// acquiring backing or transaction metadata. With growth enabled, native
// acquisition and rollback run on the captured maintenance owner; the caller
// joins that cold work outside its submission critical section. This never
// waits for another allocation to release capacity.
//
// This is the simplest possible pool. It provides the same behavior as direct
// allocation through the current iree_hal_allocator_t and serves as a baseline
// for benchmarking suballocating pool types.
//
// |slab_provider| is retained for the lifetime of the pool.
// |maintenance| is retained and shared with other pools in the memory domain.
// Final destruction joins only this pool's maintenance after the caller has
// retired its execution. Destruction runs outside that owner's executor.
// |notification| is retained for the lifetime of the pool, published
// on reservation release, and skips wake work when no waiter is observing it.
// |host_allocator| is used for pool metadata, reservation state, and
// materialization transaction state.
// The pool borrows |frontier_tracker| for reservation reuse dependencies. Its
// owning group must outlive the pool and all operations using it.
iree_status_t iree_hal_passthrough_pool_create(
    iree_hal_passthrough_pool_options_t options,
    iree_hal_slab_provider_t* slab_provider,
    iree_async_notification_t* notification,
    iree_async_frontier_tracker_t* frontier_tracker,
    iree_hal_memory_maintenance_t* maintenance, iree_allocator_t host_allocator,
    iree_hal_pool_t** out_pool);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // IREE_HAL_MEMORY_PASSTHROUGH_POOL_H_
