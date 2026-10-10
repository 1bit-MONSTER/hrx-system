// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/hal/memory/passthrough_pool.h"

#include "iree/async/frontier_tracker.h"
#include "iree/async/notification.h"
#include "iree/base/internal/math.h"
#include "iree/base/threading/notification.h"
#include "iree/hal/memory/tracing.h"

enum { IREE_HAL_PASSTHROUGH_POOL_INLINE_TRANSACTION_CAPACITY = 8 };

//===----------------------------------------------------------------------===//
// Types
//===----------------------------------------------------------------------===//

typedef struct iree_hal_passthrough_pool_reservation_state_t
    iree_hal_passthrough_pool_reservation_state_t;
typedef struct iree_hal_passthrough_pool_allocation_state_t
    iree_hal_passthrough_pool_allocation_state_t;

// Queue identities may be registered after allocation. Capture the complete
// representable release history without allocating on the release path.
IREE_ASYNC_FIXED_FRONTIER_TYPE(iree_hal_passthrough_pool_frontier_t, UINT8_MAX);

typedef enum iree_hal_passthrough_pool_retirement_state_e {
  IREE_HAL_PASSTHROUGH_POOL_RETIREMENT_LIVE = 0,
  IREE_HAL_PASSTHROUGH_POOL_RETIREMENT_WAITING,
  IREE_HAL_PASSTHROUGH_POOL_RETIREMENT_QUEUED,
  IREE_HAL_PASSTHROUGH_POOL_RETIREMENT_QUARANTINED,
} iree_hal_passthrough_pool_retirement_state_t;

typedef struct iree_hal_passthrough_pool_t {
  // Base pool resource for vtable dispatch and ref counting.
  iree_hal_pool_t base;

  // Provider used to acquire one slab per reservation.
  iree_hal_slab_provider_t* slab_provider;

  // Retained placement-local owner of native release work.
  iree_hal_memory_maintenance_t* maintenance;

  // Protects state links, waiter disposition and shutdown publication.
  iree_slim_mutex_t retirement_mutex;

  // Every native allocation still owned, including pending retirement.
  iree_hal_passthrough_pool_allocation_state_t* allocation_head;

  // Publishes native retirement to final pool teardown.
  iree_notification_t retirement_notification;

  // True after the caller has retired execution and begun pool teardown.
  bool shutting_down;

  // Tracker callbacks that may still publish work; protected by the mutex.
  iree_host_size_t pending_callbacks;

  // First failed retirement prerequisite, owned until pool destruction.
  iree_atomic_intptr_t failure_status;

  // Host allocator used for pool metadata and reservation state.
  iree_allocator_t host_allocator;

  // Stable named-memory stream for logical reservations from this pool.
  iree_hal_memory_trace_t trace;

  // Immutable memory properties provided by |slab_provider|.
  iree_hal_slab_provider_properties_t slab_properties;

  // ASAN policy used to shape hidden backing ranges.
  iree_hal_asan_pool_options_t asan_options;

  // Whether reservations may acquire new provider-owned slabs.
  bool owned_enabled;

  // Number of exact external storage classes admitted for import.
  iree_host_size_t import_class_count;

  // Immutable admitted classes stored inline after this object.
  const iree_hal_external_storage_class_t* import_classes;

  // Approximate live reservation bytes for lock-free stats queries.
  iree_atomic_int64_t bytes_reserved;

  // Native bytes still held, including pending or quarantined returns.
  iree_atomic_int64_t bytes_committed;

  // Approximate live reservation count for lock-free stats queries.
  iree_atomic_int32_t reservation_count;

  // Approximate live slab count for lock-free stats queries.
  iree_atomic_int32_t slab_count;

  // Native allocation states still owned, including external attachments.
  iree_atomic_int32_t allocation_count;

  // Total reservations committed by successful transactions.
  iree_atomic_int64_t reserve_count;

  // Total reservations returned by release transactions.
  iree_atomic_int64_t release_count;

  // Reservation transactions deferred because backing growth was prohibited.
  iree_atomic_int64_t exhausted_count;
} iree_hal_passthrough_pool_t;

// Common native allocation state retained until placement-local release.
struct iree_hal_passthrough_pool_allocation_state_t {
  // Borrowed from the source pool. Pool owners must keep the pool alive until
  // all reservations and buffers sourced from it are destroyed.
  iree_hal_pool_t* pool;

  // Slab acquired from the pool's provider for this reservation.
  iree_hal_slab_t slab;

  // Prepared facts shared by views while this slab allocation is owned.
  iree_hal_slab_buffer_backing_t buffer_backing;

  // Backing bytes charged to this reservation.
  iree_device_size_t charged_length;

  // Move-owned external transport released after the native attachment.
  iree_hal_external_storage_t external_storage;

  // Previous native allocation in the pool's retirement inventory.
  iree_hal_passthrough_pool_allocation_state_t* previous;

  // Next native allocation in the pool's retirement inventory.
  iree_hal_passthrough_pool_allocation_state_t* next;

  // Protected by the pool's retirement mutex after the allocation returns.
  iree_hal_passthrough_pool_retirement_state_t retirement_state;

  // Prepared native release entry, published only after actual completion.
  iree_hal_memory_maintenance_entry_t maintenance_entry;
};

// Per-reservation state owned by the reservation until release.
struct iree_hal_passthrough_pool_reservation_state_t {
  // Common native allocation ownership and retirement state.
  iree_hal_passthrough_pool_allocation_state_t allocation;

  // ASAN backing layout for this reservation.
  iree_hal_asan_allocation_layout_t asan_layout;

  // One reference for the live reservation token plus one reference for each
  // materialized buffer view. The last reference starts frontier retirement,
  // allowing the reservation to return before borrowed views are decommitted.
  iree_atomic_int32_t reference_count;

  // Set exactly once when the reservation token is released. Owning
  // materialized buffers consume that reservation release in their destroy
  // callback; borrowed views only drop their own reference.
  iree_atomic_int32_t reservation_released;

  // Caller-owned storage registered with the group's existing tracker.
  iree_async_frontier_waiter_t waiter;

  // Exact history copied when the explicit reservation token is released.
  iree_hal_passthrough_pool_frontier_t death_frontier;
};

typedef struct iree_hal_passthrough_pool_materialize_state_t
    iree_hal_passthrough_pool_materialize_state_t;

// Per-buffer element in an owning materialization transaction.
typedef struct iree_hal_passthrough_pool_materialize_element_t {
  // Shared transaction state controlling the ownership commit.
  iree_hal_passthrough_pool_materialize_state_t* state;

  // Reservation state released when the committed buffer is destroyed.
  iree_hal_passthrough_pool_reservation_state_t* reservation_state;

  // Materialized buffer staged until the complete transaction succeeds.
  iree_hal_buffer_t* buffer;
} iree_hal_passthrough_pool_materialize_element_t;

// Shared state for an owning materialization transaction.
struct iree_hal_passthrough_pool_materialize_state_t {
  // Host allocator used for this state object.
  iree_allocator_t host_allocator;

  // Number of materialized buffers still referencing this transaction.
  iree_atomic_int32_t reference_count;

  // True after every buffer was materialized and reservation ownership moved.
  bool ownership_committed;

  // Per-buffer transaction elements.
  iree_hal_passthrough_pool_materialize_element_t elements[];
};

// Staged result for one reservation acquisition. Transactions use staging so
// public output arrays remain untouched unless the operation succeeds.
typedef struct iree_hal_passthrough_pool_acquire_element_t {
  // Reservation produced for the request.
  iree_hal_pool_reservation_t reservation;

  // Acquisition metadata produced for the request.
  iree_hal_pool_acquire_info_t info;
} iree_hal_passthrough_pool_acquire_element_t;

static const iree_hal_pool_vtable_t iree_hal_passthrough_pool_vtable;
static void iree_hal_passthrough_pool_destroy(iree_hal_pool_t* base_pool);

static const char* IREE_HAL_PASSTHROUGH_POOL_TRACE_ID =
    "iree-hal-passthrough-pool";

// Returns a process-local tracing identity without interpreting opaque device
// storage as an address. Device providers may intentionally leave base_ptr
// null and use only provider_handle.
static void* iree_hal_passthrough_pool_trace_address(
    const iree_hal_slab_t* slab, iree_device_size_t offset) {
  uintptr_t base = (uintptr_t)slab->base_ptr;
  if (!base) {
    base = (uintptr_t)slab->provider_handle;
  }
  return base ? (void*)(base + (uintptr_t)offset) : NULL;
}

//===----------------------------------------------------------------------===//
// Reservation state helpers
//===----------------------------------------------------------------------===//

// Unlinks metadata after its native resource has been released. slab_count
// remains charged until metadata freeing finishes, keeping teardown joined.
static void iree_hal_passthrough_pool_unlink_allocation(
    iree_hal_passthrough_pool_t* pool,
    iree_hal_passthrough_pool_allocation_state_t* state) {
  iree_slim_mutex_lock(&pool->retirement_mutex);
  if (state->previous) {
    state->previous->next = state->next;
  } else {
    pool->allocation_head = state->next;
  }
  if (state->next) {
    state->next->previous = state->previous;
  }
  iree_slim_mutex_unlock(&pool->retirement_mutex);
}

static void iree_hal_passthrough_pool_retire_allocation(
    iree_hal_memory_maintenance_entry_t* entry) {
  iree_hal_passthrough_pool_allocation_state_t* state =
      (iree_hal_passthrough_pool_allocation_state_t*)((uint8_t*)entry -
                                                      offsetof(
                                                          iree_hal_passthrough_pool_allocation_state_t,
                                                          maintenance_entry));
  iree_hal_passthrough_pool_t* pool = (iree_hal_passthrough_pool_t*)state->pool;
  const iree_allocator_t host_allocator = pool->host_allocator;
  iree_hal_slab_provider_release_slab(pool->slab_provider, &state->slab);
  iree_hal_external_storage_release(&state->external_storage);
  const iree_device_size_t charged_length = state->charged_length;
  iree_hal_passthrough_pool_unlink_allocation(pool, state);
  iree_allocator_free(host_allocator, state);
  iree_slim_mutex_lock(&pool->retirement_mutex);
  if (charged_length) {
    iree_atomic_fetch_sub(&pool->bytes_committed, (int64_t)charged_length,
                          iree_memory_order_relaxed);
    iree_atomic_fetch_sub(&pool->slab_count, 1, iree_memory_order_relaxed);
  }
  iree_atomic_fetch_sub(&pool->allocation_count, 1, iree_memory_order_relaxed);
  iree_notification_post(&pool->retirement_notification, IREE_ALL_WAITERS);
  iree_slim_mutex_unlock(&pool->retirement_mutex);
}

static void iree_hal_passthrough_pool_frontier_resolved(void* user_data,
                                                        iree_status_t status) {
  iree_hal_passthrough_pool_reservation_state_t* reservation_state =
      (iree_hal_passthrough_pool_reservation_state_t*)user_data;
  iree_hal_passthrough_pool_allocation_state_t* state =
      &reservation_state->allocation;
  iree_hal_passthrough_pool_t* pool = (iree_hal_passthrough_pool_t*)state->pool;
  const bool failed = !iree_status_is_ok(status);
  iree_slim_mutex_lock(&pool->retirement_mutex);
  const bool may_retire = !failed || pool->shutting_down;
  if (failed) {
    const iree_status_t failure = (iree_status_t)iree_atomic_load(
        &pool->failure_status, iree_memory_order_relaxed);
    if (iree_status_is_ok(failure)) {
      iree_atomic_store(&pool->failure_status, (intptr_t)status,
                        iree_memory_order_release);
      status = iree_ok_status();
    }
  }
  state->retirement_state =
      may_retire ? IREE_HAL_PASSTHROUGH_POOL_RETIREMENT_QUEUED
                 : IREE_HAL_PASSTHROUGH_POOL_RETIREMENT_QUARANTINED;
  iree_slim_mutex_unlock(&pool->retirement_mutex);
  // The first terminal failure owns the diagnostic. Dispose of subsequent
  // callback statuses outside the metadata lock.
  iree_status_free(status);
  if (may_retire) {
    iree_hal_memory_maintenance_enqueue(pool->maintenance,
                                        &state->maintenance_entry);
  }
  if (failed) {
    iree_async_notification_signal_if_observed(pool->base.notification,
                                               INT32_MAX);
  }
  // The entry can already have been freed by the worker. Keep only the pool
  // borrow until enqueue has finished accessing its maintenance owner.
  iree_slim_mutex_lock(&pool->retirement_mutex);
  --pool->pending_callbacks;
  iree_notification_post(&pool->retirement_notification, IREE_ALL_WAITERS);
  iree_slim_mutex_unlock(&pool->retirement_mutex);
}

static void iree_hal_passthrough_pool_reservation_state_release_reference(
    iree_hal_passthrough_pool_reservation_state_t* reservation_state) {
  iree_hal_passthrough_pool_allocation_state_t* allocation =
      &reservation_state->allocation;
  iree_hal_passthrough_pool_t* pool =
      (iree_hal_passthrough_pool_t*)allocation->pool;
  const int32_t previous_count = iree_atomic_fetch_sub(
      &reservation_state->reference_count, 1, iree_memory_order_acq_rel);
  IREE_ASSERT(previous_count > 0);
  if (previous_count != 1) {
    return;
  }

  iree_slim_mutex_lock(&pool->retirement_mutex);
  allocation->retirement_state = IREE_HAL_PASSTHROUGH_POOL_RETIREMENT_WAITING;
  ++pool->pending_callbacks;
  iree_slim_mutex_unlock(&pool->retirement_mutex);
  iree_status_t status = iree_async_frontier_tracker_wait(
      pool->base.frontier_tracker,
      iree_async_fixed_frontier_as_const_frontier(
          &reservation_state->death_frontier),
      iree_hal_passthrough_pool_frontier_resolved, reservation_state,
      &reservation_state->waiter);
  if (!iree_status_is_ok(status)) {
    iree_hal_passthrough_pool_frontier_resolved(reservation_state, status);
  }
}

static void iree_hal_passthrough_pool_reservation_state_release_reservation(
    iree_hal_passthrough_pool_reservation_state_t* reservation_state,
    const iree_async_frontier_t* death_frontier) {
  iree_hal_passthrough_pool_allocation_state_t* allocation =
      &reservation_state->allocation;
  iree_hal_passthrough_pool_t* pool =
      (iree_hal_passthrough_pool_t*)allocation->pool;
  const int32_t already_released = iree_atomic_exchange(
      &reservation_state->reservation_released, 1, iree_memory_order_acq_rel);
  IREE_ASSERT_EQ(already_released, 0);

  if (death_frontier) {
    memcpy(&reservation_state->death_frontier, death_frontier,
           sizeof(*death_frontier) + death_frontier->entry_count *
                                         sizeof(death_frontier->entries[0]));
  }

  const iree_device_size_t user_offset =
      iree_hal_asan_pool_options_is_enabled(&pool->asan_options)
          ? reservation_state->asan_layout.user_offset
          : 0;
  iree_hal_memory_trace_free(
      &pool->trace,
      iree_hal_passthrough_pool_trace_address(&allocation->slab, user_offset));
  iree_atomic_fetch_add(&pool->bytes_reserved,
                        -(int64_t)allocation->charged_length,
                        iree_memory_order_relaxed);
  iree_atomic_fetch_add(&pool->reservation_count, -1,
                        iree_memory_order_relaxed);
  iree_atomic_fetch_add(&pool->release_count, 1, iree_memory_order_relaxed);

  iree_hal_passthrough_pool_reservation_state_release_reference(
      reservation_state);
}

static void iree_hal_passthrough_pool_borrowed_view_release(
    void* user_data, iree_hal_buffer_t* buffer) {
  (void)buffer;
  iree_hal_passthrough_pool_reservation_state_t* reservation_state =
      (iree_hal_passthrough_pool_reservation_state_t*)user_data;
  iree_hal_passthrough_pool_reservation_state_release_reference(
      reservation_state);
}

static void iree_hal_passthrough_pool_advise_asan_reservations(
    iree_hal_pool_t* base_pool, iree_host_size_t reservation_count,
    const iree_hal_pool_reservation_t* reservations,
    iree_hal_asan_range_advice_flags_t flags) {
  iree_hal_passthrough_pool_t* pool = (iree_hal_passthrough_pool_t*)base_pool;
  for (iree_host_size_t i = 0; i < reservation_count; ++i) {
    iree_hal_passthrough_pool_reservation_state_t* state =
        (iree_hal_passthrough_pool_reservation_state_t*)(uintptr_t)
            reservations[i]
                .block_handle;
    iree_hal_slab_provider_advise_asan_range(pool->slab_provider,
                                             &state->allocation.slab, 0, flags,
                                             &state->asan_layout);
  }
}

static void iree_hal_passthrough_pool_owned_buffer_release(
    void* user_data, iree_hal_buffer_t* buffer) {
  (void)buffer;
  iree_hal_passthrough_pool_materialize_element_t* element =
      (iree_hal_passthrough_pool_materialize_element_t*)user_data;
  iree_hal_passthrough_pool_materialize_state_t* state = element->state;
  iree_hal_passthrough_pool_reservation_state_t* reservation_state =
      element->reservation_state;
  if (state->ownership_committed) {
    const iree_hal_pool_reservation_t reservation = {
        .block_handle = (uint64_t)(uintptr_t)reservation_state,
    };
    iree_hal_pool_advise_asan_reservations(
        reservation_state->allocation.pool, 1, &reservation,
        IREE_HAL_ASAN_RANGE_ADVICE_FLAG_RELEASED);
    iree_hal_passthrough_pool_reservation_state_release_reservation(
        reservation_state, NULL);
    iree_hal_passthrough_pool_t* pool =
        (iree_hal_passthrough_pool_t*)reservation_state->allocation.pool;
    iree_async_notification_signal_if_observed(pool->base.notification,
                                               INT32_MAX);
  }
  iree_hal_passthrough_pool_reservation_state_release_reference(
      reservation_state);
  const int32_t previous_count = iree_atomic_fetch_sub(
      &state->reference_count, 1, iree_memory_order_acq_rel);
  IREE_ASSERT(previous_count > 0);
  if (previous_count == 1) {
    iree_allocator_free(state->host_allocator, state);
  }
}

//===----------------------------------------------------------------------===//
// Create / Destroy
//===----------------------------------------------------------------------===//

iree_status_t iree_hal_passthrough_pool_create(
    iree_hal_passthrough_pool_options_t options,
    iree_hal_slab_provider_t* slab_provider,
    iree_async_notification_t* notification,
    iree_async_frontier_tracker_t* frontier_tracker,
    iree_hal_memory_maintenance_t* maintenance, iree_allocator_t host_allocator,
    iree_hal_pool_t** out_pool) {
  IREE_ASSERT_ARGUMENT(slab_provider);
  IREE_ASSERT_ARGUMENT(notification);
  IREE_ASSERT_ARGUMENT(maintenance);
  IREE_ASSERT_ARGUMENT(out_pool);
  *out_pool = NULL;
  IREE_TRACE_ZONE_BEGIN(z0);

  IREE_RETURN_AND_END_ZONE_IF_ERROR(
      z0, iree_hal_slab_provider_validate_asan_options(slab_provider,
                                                       &options.asan));

  iree_host_size_t pool_size = 0;
  IREE_RETURN_AND_END_ZONE_IF_ERROR(
      z0, iree_host_size_checked_mul_add(
              options.imports.class_count,
              sizeof(iree_hal_external_storage_class_t),
              sizeof(iree_hal_passthrough_pool_t), &pool_size)
              ? iree_ok_status()
              : iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                                 "pass-through pool metadata size overflow"));
  iree_hal_passthrough_pool_t* pool = NULL;
  IREE_RETURN_AND_END_ZONE_IF_ERROR(
      z0, iree_allocator_malloc(host_allocator, pool_size, (void**)&pool));
  memset(pool, 0, pool_size);
  iree_status_t status = iree_hal_pool_initialize(
      &iree_hal_passthrough_pool_vtable, options.memory_contract, notification,
      (iree_hal_pool_wait_source_list_t){0}, frontier_tracker, host_allocator,
      &pool->base);
  if (!iree_status_is_ok(status)) {
    iree_allocator_free(host_allocator, pool);
    IREE_TRACE_ZONE_END(z0);
    return status;
  }
  pool->host_allocator = host_allocator;
  pool->base.epoch_query = options.epoch_query;
  pool->asan_options = options.asan;
  pool->owned_enabled = options.owned_mode == IREE_HAL_SLAB_POOL_OWNED_ENABLED;
  pool->import_class_count = options.imports.class_count;
  pool->import_classes = (const iree_hal_external_storage_class_t*)(pool + 1);
  iree_hal_external_storage_class_t* import_classes =
      (iree_hal_external_storage_class_t*)pool->import_classes;
  for (iree_host_size_t i = 0; i < options.imports.class_count; ++i) {
    import_classes[i] = options.imports.classes[i].storage_class;
  }
  pool->base.asan_enabled =
      iree_hal_asan_pool_options_is_enabled(&options.asan);
  pool->base.maintenance = maintenance;
  pool->maintenance = maintenance;
  iree_hal_memory_maintenance_retain(maintenance);
  iree_slim_mutex_initialize(&pool->retirement_mutex);
  iree_notification_initialize(&pool->retirement_notification);

  iree_hal_slab_provider_retain(slab_provider);
  pool->slab_provider = slab_provider;

  iree_hal_slab_provider_query_properties(slab_provider,
                                          &pool->slab_properties);

  status = iree_hal_memory_trace_initialize_pool(
      options.trace_name, IREE_HAL_PASSTHROUGH_POOL_TRACE_ID, host_allocator,
      &pool->trace);
  if (iree_status_is_ok(status)) {
    *out_pool = (iree_hal_pool_t*)pool;
  } else {
    iree_hal_passthrough_pool_destroy((iree_hal_pool_t*)pool);
  }
  IREE_TRACE_ZONE_END(z0);
  return status;
}

static bool iree_hal_passthrough_pool_retirement_is_complete(void* user_data) {
  iree_hal_passthrough_pool_t* pool = (iree_hal_passthrough_pool_t*)user_data;
  iree_slim_mutex_lock(&pool->retirement_mutex);
  const bool complete =
      pool->pending_callbacks == 0 &&
      iree_atomic_load(&pool->allocation_count, iree_memory_order_relaxed) == 0;
  iree_slim_mutex_unlock(&pool->retirement_mutex);
  return complete;
}

static void iree_hal_passthrough_pool_destroy(iree_hal_pool_t* base_pool) {
  IREE_TRACE_ZONE_BEGIN(z0);
  iree_hal_passthrough_pool_t* pool = (iree_hal_passthrough_pool_t*)base_pool;
  iree_allocator_t host_allocator = pool->host_allocator;
  iree_hal_memory_maintenance_entry_t* ready_head = NULL;
  iree_slim_mutex_lock(&pool->retirement_mutex);
  pool->shutting_down = true;
  for (iree_hal_passthrough_pool_allocation_state_t* state =
           pool->allocation_head;
       state; state = state->next) {
    IREE_ASSERT(
        state->retirement_state != IREE_HAL_PASSTHROUGH_POOL_RETIREMENT_LIVE,
        "pool destruction requires returned reservations and views");
    if (state->retirement_state ==
        IREE_HAL_PASSTHROUGH_POOL_RETIREMENT_WAITING) {
      iree_hal_passthrough_pool_reservation_state_t* reservation_state =
          (iree_hal_passthrough_pool_reservation_state_t*)state;
      if (iree_async_frontier_tracker_cancel_wait(pool->base.frontier_tracker,
                                                  &reservation_state->waiter)) {
        --pool->pending_callbacks;
        state->retirement_state =
            IREE_HAL_PASSTHROUGH_POOL_RETIREMENT_QUARANTINED;
      }
    }
    // Destruction's caller-quiescence contract permits cleanup after a failed
    // or cancelled wait. An ordinary release/trim cannot make this inference.
    if (state->retirement_state ==
        IREE_HAL_PASSTHROUGH_POOL_RETIREMENT_QUARANTINED) {
      state->retirement_state = IREE_HAL_PASSTHROUGH_POOL_RETIREMENT_QUEUED;
      state->maintenance_entry.next = ready_head;
      ready_head = &state->maintenance_entry;
    }
  }
  iree_slim_mutex_unlock(&pool->retirement_mutex);
  while (ready_head) {
    iree_hal_memory_maintenance_entry_t* entry = ready_head;
    ready_head = entry->next;
    iree_hal_memory_maintenance_enqueue(pool->maintenance, entry);
  }
  iree_notification_await(&pool->retirement_notification,
                          iree_hal_passthrough_pool_retirement_is_complete,
                          pool, iree_infinite_timeout());
  iree_hal_memory_trace_deinitialize(&pool->trace);
  iree_hal_pool_deinitialize(base_pool);
  iree_hal_slab_provider_release(pool->slab_provider);
  iree_hal_memory_maintenance_release(pool->maintenance);
  iree_status_free((iree_status_t)iree_atomic_load(&pool->failure_status,
                                                   iree_memory_order_relaxed));
  iree_notification_deinitialize(&pool->retirement_notification);
  iree_slim_mutex_deinitialize(&pool->retirement_mutex);
  iree_allocator_free(host_allocator, pool);
  IREE_TRACE_ZONE_END(z0);
}

//===----------------------------------------------------------------------===//
// External storage import
//===----------------------------------------------------------------------===//

static bool iree_hal_passthrough_pool_supports_import_class(
    const iree_hal_pool_t* base_pool,
    iree_hal_external_storage_class_t storage_class) {
  const iree_hal_passthrough_pool_t* pool =
      (const iree_hal_passthrough_pool_t*)base_pool;
  for (iree_host_size_t i = 0; i < pool->import_class_count; ++i) {
    if (iree_hal_external_storage_class_equal(pool->import_classes[i],
                                              storage_class)) {
      return true;
    }
  }
  return false;
}

static bool iree_hal_passthrough_pool_query_import_requirements(
    const iree_hal_pool_t* base_pool,
    iree_hal_external_storage_class_t storage_class,
    iree_hal_pool_import_requirements_t* out_requirements) {
  const iree_hal_passthrough_pool_t* pool =
      (const iree_hal_passthrough_pool_t*)base_pool;
  if (!iree_hal_passthrough_pool_supports_import_class(base_pool,
                                                       storage_class)) {
    return false;
  }
  return iree_hal_slab_provider_query_import_requirements(
      pool->slab_provider, storage_class, out_requirements);
}

typedef struct iree_hal_passthrough_pool_import_call_t {
  // Native pool whose placement-local owner executes attachment.
  iree_hal_passthrough_pool_t* pool;
  // Borrowed external storage valid until the joined call returns.
  const iree_hal_external_storage_t* storage;
  // Staged native attachment published only after successful return.
  iree_hal_imported_slab_t imported_slab;
  // Native attachment failure returned to the caller.
  iree_status_t status;
} iree_hal_passthrough_pool_import_call_t;

static void iree_hal_passthrough_pool_import_on_owner(void* user_data) {
  iree_hal_passthrough_pool_import_call_t* call = user_data;
  call->status = iree_hal_slab_provider_import_slab(
      call->pool->slab_provider, call->storage, &call->imported_slab);
}

typedef struct iree_hal_passthrough_pool_import_rollback_call_t {
  // Provider that owns the staged native attachment.
  iree_hal_slab_provider_t* provider;
  // Staged attachment released before returning to the failed importer.
  iree_hal_slab_t slab;
} iree_hal_passthrough_pool_import_rollback_call_t;

static void iree_hal_passthrough_pool_import_rollback_on_owner(
    void* user_data) {
  iree_hal_passthrough_pool_import_rollback_call_t* call = user_data;
  iree_hal_slab_provider_release_slab(call->provider, &call->slab);
}

static void iree_hal_passthrough_pool_imported_buffer_release(
    void* user_data, iree_hal_buffer_t* buffer) {
  (void)buffer;
  iree_hal_passthrough_pool_allocation_state_t* allocation = user_data;
  iree_hal_passthrough_pool_t* pool =
      (iree_hal_passthrough_pool_t*)allocation->pool;
  iree_hal_memory_trace_free(&pool->trace,
                             iree_hal_passthrough_pool_trace_address(
                                 &allocation->slab, buffer->byte_offset));
  iree_slim_mutex_lock(&pool->retirement_mutex);
  IREE_ASSERT_EQ(allocation->retirement_state,
                 IREE_HAL_PASSTHROUGH_POOL_RETIREMENT_LIVE);
  allocation->retirement_state = IREE_HAL_PASSTHROUGH_POOL_RETIREMENT_QUEUED;
  iree_slim_mutex_unlock(&pool->retirement_mutex);
  iree_hal_memory_maintenance_enqueue(pool->maintenance,
                                      &allocation->maintenance_entry);
}

static iree_status_t iree_hal_passthrough_pool_validate_imported_slab(
    const iree_hal_passthrough_pool_t* pool,
    const iree_hal_external_storage_t* storage,
    const iree_hal_imported_slab_t* imported_slab) {
  const iree_hal_slab_provider_properties_t* properties =
      &imported_slab->properties;
  if (imported_slab->slab.length != storage->byte_length) {
    return iree_make_status(
        IREE_STATUS_FAILED_PRECONDITION,
        "native import changed the external storage extent");
  }
  if (!iree_device_size_is_power_of_two(properties->allocation_alignment) ||
      !iree_device_size_is_power_of_two(properties->maintenance_alignment)) {
    return iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                            "native import returned invalid alignment facts");
  }
  const iree_hal_atomic_operation_capabilities_t required_atomics =
      pool->slab_properties.atomic_operations;
  const iree_hal_atomic_operation_capabilities_t actual_atomics =
      properties->atomic_operations;
  const bool supports_atomics = iree_hal_atomic_operation_capabilities_support(
      actual_atomics, required_atomics);
  const iree_hal_buffer_usage_t required_usage =
      pool->base.memory_contract ? pool->base.memory_contract->allowed_usage
                                 : pool->slab_properties.supported_usage;
  if (!iree_all_bits_set(properties->memory_type,
                         pool->slab_properties.memory_type) ||
      !iree_all_bits_set(properties->supported_usage, required_usage) ||
      !iree_all_bits_set(properties->queue_family_affinity,
                         pool->slab_properties.queue_family_affinity) ||
      !supports_atomics) {
    return iree_make_status(
        IREE_STATUS_FAILED_PRECONDITION,
        "external storage does not satisfy the pool memory contract");
  }
  return iree_ok_status();
}

static iree_status_t iree_hal_passthrough_pool_import_buffer(
    iree_hal_pool_t* base_pool, iree_hal_external_storage_t* inout_storage,
    iree_hal_buffer_range_t logical_range, iree_hal_buffer_t** out_buffer) {
  iree_hal_passthrough_pool_t* pool = (iree_hal_passthrough_pool_t*)base_pool;
  if (!iree_hal_passthrough_pool_supports_import_class(
          base_pool, inout_storage->storage_class)) {
    return iree_make_status(IREE_STATUS_UNAVAILABLE,
                            "external storage class was not admitted by pool");
  }
  if (pool->base.asan_enabled) {
    return iree_make_status(
        IREE_STATUS_FAILED_PRECONDITION,
        "external storage cannot satisfy pass-through ASAN layout policy");
  }

  iree_hal_passthrough_pool_allocation_state_t* allocation = NULL;
  IREE_RETURN_IF_ERROR(iree_allocator_malloc(
      pool->host_allocator, sizeof(*allocation), (void**)&allocation));
  memset(allocation, 0, sizeof(*allocation));
  allocation->pool = base_pool;

  iree_hal_passthrough_pool_import_call_t call = {
      .pool = pool,
      .storage = inout_storage,
  };
  iree_hal_memory_maintenance_call(
      pool->maintenance, iree_hal_passthrough_pool_import_on_owner, &call);
  iree_status_t status = call.status;
  const bool native_attached = iree_status_is_ok(status);
  if (iree_status_is_ok(status)) {
    status = iree_hal_passthrough_pool_validate_imported_slab(
        pool, inout_storage, &call.imported_slab);
  }

  iree_hal_buffer_t* buffer = NULL;
  if (iree_status_is_ok(status)) {
    allocation->slab = call.imported_slab.slab;
    iree_hal_buffer_range_t maintenance_range = {0};
    status = iree_hal_buffer_calculate_range(
        0, inout_storage->byte_length,
        inout_storage->maintenance_range.byte_offset,
        inout_storage->maintenance_range.byte_length,
        &maintenance_range.byte_offset, &maintenance_range.byte_length);
    if (iree_status_is_ok(status)) {
      iree_hal_slab_buffer_backing_initialize(
          pool->slab_provider, &allocation->slab,
          &call.imported_slab.properties, maintenance_range,
          inout_storage->host_cacheability, inout_storage->physical_backing_id,
          pool->base.notification, pool->base.frontier_tracker,
          pool->maintenance, &allocation->buffer_backing);
      allocation->maintenance_entry.fn =
          iree_hal_passthrough_pool_retire_allocation;
      const iree_hal_buffer_release_callback_t release_callback = {
          .fn = iree_hal_passthrough_pool_imported_buffer_release,
          .user_data = allocation,
      };
      const iree_hal_memory_access_t allowed_access =
          base_pool->memory_contract
              ? base_pool->memory_contract->allowed_access
              : IREE_HAL_MEMORY_ACCESS_ALL;
      const iree_hal_buffer_usage_t allowed_usage =
          base_pool->memory_contract
              ? base_pool->memory_contract->allowed_usage
              : call.imported_slab.properties.supported_usage;
      status = iree_hal_slab_provider_wrap_buffer(
          pool->slab_provider, &allocation->slab, logical_range.byte_offset,
          logical_range.byte_length, allowed_access, allowed_usage,
          release_callback, &buffer);
    }
  }

  if (iree_status_is_ok(status)) {
    buffer->memory.contract = base_pool->memory_contract;
    buffer->memory.backing = &allocation->buffer_backing.facts;
    buffer->memory.offset = logical_range.byte_offset;
    allocation->external_storage = *inout_storage;
    memset(inout_storage, 0, sizeof(*inout_storage));
    iree_atomic_fetch_add(&pool->allocation_count, 1,
                          iree_memory_order_relaxed);
    iree_slim_mutex_lock(&pool->retirement_mutex);
    allocation->next = pool->allocation_head;
    if (pool->allocation_head) {
      pool->allocation_head->previous = allocation;
    }
    pool->allocation_head = allocation;
    iree_slim_mutex_unlock(&pool->retirement_mutex);
    iree_hal_memory_trace_alloc(
        &pool->trace,
        iree_hal_passthrough_pool_trace_address(&allocation->slab,
                                                logical_range.byte_offset),
        logical_range.byte_length);
    *out_buffer = buffer;
  } else {
    if (native_attached) {
      iree_hal_passthrough_pool_import_rollback_call_t rollback_call = {
          .provider = pool->slab_provider,
          .slab = call.imported_slab.slab,
      };
      iree_hal_memory_maintenance_call(
          pool->maintenance, iree_hal_passthrough_pool_import_rollback_on_owner,
          &rollback_call);
    }
    iree_allocator_free(pool->host_allocator, allocation);
  }
  return status;
}

//===----------------------------------------------------------------------===//
// Reserve / Release
//===----------------------------------------------------------------------===//

static iree_status_t iree_hal_passthrough_pool_validate_reservation_request(
    const iree_hal_passthrough_pool_t* pool,
    const iree_hal_pool_reservation_request_t* request) {
  const iree_device_size_t size = request->allocation_size;
  const iree_device_size_t alignment =
      request->min_alignment ? request->min_alignment : 1;
  if (size == 0) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "reservation size must be > 0");
  }
  if (!iree_device_size_is_power_of_two(alignment)) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "reservation alignment (%" PRIdsz
                            ") must be a power of two",
                            alignment);
  }
  if (alignment > pool->slab_properties.max_allocation_alignment) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "reservation alignment %" PRIdsz
                            " exceeds pass-through pool alignment %" PRIdsz,
                            alignment,
                            pool->slab_properties.max_allocation_alignment);
  }
  if (iree_hal_asan_pool_options_is_enabled(&pool->asan_options)) {
    iree_hal_asan_allocation_layout_t asan_layout;
    IREE_RETURN_IF_ERROR(iree_hal_asan_calculate_allocation_layout(
        &pool->asan_options, size, alignment, &asan_layout));
  }
  return iree_ok_status();
}

static iree_status_t iree_hal_passthrough_pool_acquire_one_reservation(
    iree_hal_pool_t* base_pool,
    const iree_hal_pool_reservation_request_t* request,
    iree_hal_pool_reservation_t* out_reservation,
    iree_hal_pool_acquire_info_t* out_info) {
  iree_hal_passthrough_pool_t* pool = (iree_hal_passthrough_pool_t*)base_pool;
  const iree_device_size_t size = request->allocation_size;
  const iree_device_size_t alignment =
      request->min_alignment ? request->min_alignment : 1;

  iree_hal_asan_allocation_layout_t asan_layout = {0};
  iree_device_size_t backing_length = size;
  if (iree_hal_asan_pool_options_is_enabled(&pool->asan_options)) {
    IREE_RETURN_IF_ERROR(iree_hal_asan_calculate_allocation_layout(
        &pool->asan_options, size, alignment, &asan_layout));
    backing_length = asan_layout.backing_length;
  }

  iree_hal_slab_t slab;
  const iree_device_size_t backing_alignment =
      iree_max(alignment, asan_layout.backing_offset_alignment);
  IREE_RETURN_IF_ERROR(iree_hal_slab_provider_acquire_slab(
      pool->slab_provider, backing_length, backing_alignment, &slab));
  if (iree_hal_asan_pool_options_is_enabled(&pool->asan_options) &&
      slab.length != asan_layout.backing_length) {
    iree_status_t status =
        iree_hal_asan_extend_allocation_layout(slab.length, &asan_layout);
    if (!iree_status_is_ok(status)) {
      iree_hal_slab_provider_release_slab(pool->slab_provider, &slab);
      return status;
    }
  }

  iree_hal_passthrough_pool_reservation_state_t* reservation_state = NULL;
  iree_status_t status =
      iree_allocator_malloc(pool->host_allocator, sizeof(*reservation_state),
                            (void**)&reservation_state);
  if (!iree_status_is_ok(status)) {
    iree_hal_slab_provider_release_slab(pool->slab_provider, &slab);
    return status;
  }
  memset(reservation_state, 0, sizeof(*reservation_state));
  iree_hal_passthrough_pool_allocation_state_t* allocation =
      &reservation_state->allocation;
  allocation->pool = base_pool;
  allocation->slab = slab;
  iree_hal_slab_provider_properties_t slab_properties = pool->slab_properties;
  slab_properties.allocation_alignment =
      iree_max(slab_properties.allocation_alignment, backing_alignment);
  iree_hal_slab_buffer_backing_initialize(
      pool->slab_provider, &allocation->slab, &slab_properties,
      iree_hal_make_buffer_range(0, slab.length),
      iree_any_bit_set(slab_properties.memory_type,
                       IREE_HAL_MEMORY_TYPE_HOST_CACHED)
          ? IREE_HAL_HOST_CACHEABILITY_WRITE_BACK
          : IREE_HAL_HOST_CACHEABILITY_UNKNOWN,
      (iree_hal_physical_memory_id_t){0}, pool->base.notification,
      pool->base.frontier_tracker, pool->maintenance,
      &allocation->buffer_backing);
  allocation->charged_length = slab.length;
  reservation_state->asan_layout = asan_layout;
  allocation->maintenance_entry.fn =
      iree_hal_passthrough_pool_retire_allocation;
  iree_atomic_store(&reservation_state->reference_count, 1,
                    iree_memory_order_relaxed);
  iree_atomic_store(&reservation_state->reservation_released, 0,
                    iree_memory_order_relaxed);

  memset(out_reservation, 0, sizeof(*out_reservation));
  out_reservation->offset =
      iree_hal_asan_pool_options_is_enabled(&pool->asan_options)
          ? asan_layout.user_offset
          : 0;
  out_reservation->byte_length = size;
  out_reservation->block_handle = (uint64_t)(uintptr_t)reservation_state;

  iree_atomic_fetch_add(&pool->bytes_reserved, (int64_t)slab.length,
                        iree_memory_order_relaxed);
  iree_atomic_fetch_add(&pool->bytes_committed, (int64_t)slab.length,
                        iree_memory_order_relaxed);
  iree_atomic_fetch_add(&pool->reservation_count, 1, iree_memory_order_relaxed);
  iree_atomic_fetch_add(&pool->slab_count, 1, iree_memory_order_relaxed);
  iree_atomic_fetch_add(&pool->allocation_count, 1, iree_memory_order_relaxed);
  iree_atomic_fetch_add(&pool->reserve_count, 1, iree_memory_order_relaxed);
  iree_slim_mutex_lock(&pool->retirement_mutex);
  allocation->next = pool->allocation_head;
  if (pool->allocation_head) {
    pool->allocation_head->previous = allocation;
  }
  pool->allocation_head = allocation;
  iree_slim_mutex_unlock(&pool->retirement_mutex);

  iree_hal_memory_trace_alloc(&pool->trace,
                              iree_hal_passthrough_pool_trace_address(
                                  &allocation->slab, out_reservation->offset),
                              out_reservation->byte_length);

  memset(out_info, 0, sizeof(*out_info));
  out_info->result = IREE_HAL_POOL_ACQUIRE_OK_FRESH;
  return iree_ok_status();
}

// Rolls back a reservation acquired by the current transaction before it was
// made visible to the caller.
static void iree_hal_passthrough_pool_rollback_reservation(
    iree_hal_passthrough_pool_t* pool,
    const iree_hal_pool_reservation_t* reservation) {
  iree_hal_passthrough_pool_reservation_state_t* reservation_state =
      (iree_hal_passthrough_pool_reservation_state_t*)(uintptr_t)
          reservation->block_handle;
  const iree_device_size_t user_offset =
      iree_hal_asan_pool_options_is_enabled(&pool->asan_options)
          ? reservation_state->asan_layout.user_offset
          : 0;
  iree_hal_memory_trace_free(
      &pool->trace, iree_hal_passthrough_pool_trace_address(
                        &reservation_state->allocation.slab, user_offset));
  iree_atomic_fetch_add(&pool->bytes_reserved,
                        -(int64_t)reservation_state->allocation.charged_length,
                        iree_memory_order_relaxed);
  iree_atomic_fetch_add(&pool->reservation_count, -1,
                        iree_memory_order_relaxed);
  iree_atomic_fetch_add(&pool->slab_count, -1, iree_memory_order_relaxed);
  iree_atomic_fetch_add(&pool->allocation_count, -1, iree_memory_order_relaxed);
  iree_atomic_fetch_sub(&pool->bytes_committed,
                        (int64_t)reservation_state->allocation.charged_length,
                        iree_memory_order_relaxed);
  iree_atomic_fetch_add(&pool->reserve_count, -1, iree_memory_order_relaxed);
  iree_hal_slab_provider_release_slab(pool->slab_provider,
                                      &reservation_state->allocation.slab);
  iree_hal_passthrough_pool_unlink_allocation(pool,
                                              &reservation_state->allocation);
  iree_allocator_free(pool->host_allocator, reservation_state);
}

// Cold native growth transaction. All pointers are borrowed until the owner
// call joins; output publication and rollback remain all-or-none.
typedef struct iree_hal_passthrough_pool_acquire_call_t {
  // Native pool whose placement-local owner executes the transaction.
  iree_hal_passthrough_pool_t* pool;
  // Number of requests and output slots.
  iree_host_size_t request_count;
  // Immutable geometry and permissions supplied by the caller.
  const iree_hal_pool_reservation_request_t* requests;
  // Reservations assigned only after every native acquisition succeeds.
  iree_hal_pool_reservation_t* out_reservations;
  // Exact reuse information assigned with the reservations.
  iree_hal_pool_acquire_info_t* out_infos;
  // Aggregate acquisition outcome assigned only on success.
  iree_hal_pool_acquire_result_t* out_result;
  // Owned native or host allocation failure returned to the caller.
  iree_status_t status;
} iree_hal_passthrough_pool_acquire_call_t;

static void iree_hal_passthrough_pool_acquire_on_owner(void* user_data) {
  iree_hal_passthrough_pool_acquire_call_t* call = user_data;
  iree_hal_passthrough_pool_t* pool = call->pool;
  const iree_host_size_t request_count = call->request_count;
  iree_hal_passthrough_pool_acquire_element_t
      inline_elements[IREE_HAL_PASSTHROUGH_POOL_INLINE_TRANSACTION_CAPACITY];
  iree_hal_passthrough_pool_acquire_element_t* elements = inline_elements;
  bool elements_allocated = false;
  iree_status_t status = iree_ok_status();
  if (request_count > IREE_ARRAYSIZE(inline_elements)) {
    status = iree_allocator_malloc_array(pool->host_allocator, request_count,
                                         sizeof(*elements), (void**)&elements);
    elements_allocated = iree_status_is_ok(status);
  }
  if (iree_status_is_ok(status)) {
    memset(elements, 0, request_count * sizeof(*elements));
  }

  iree_host_size_t acquired_count = 0;
  while (acquired_count < request_count && iree_status_is_ok(status)) {
    status = iree_hal_passthrough_pool_acquire_one_reservation(
        &pool->base, &call->requests[acquired_count],
        &elements[acquired_count].reservation, &elements[acquired_count].info);
    if (iree_status_is_ok(status)) {
      ++acquired_count;
    }
  }
  if (!iree_status_is_ok(status)) {
    for (iree_host_size_t i = 0; i < acquired_count; ++i) {
      iree_hal_passthrough_pool_rollback_reservation(pool,
                                                     &elements[i].reservation);
    }
    if (acquired_count != 0) {
      iree_async_notification_signal_if_observed(pool->base.notification,
                                                 INT32_MAX);
    }
  } else {
    for (iree_host_size_t i = 0; i < request_count; ++i) {
      call->out_reservations[i] = elements[i].reservation;
      call->out_infos[i] = elements[i].info;
    }
    *call->out_result = IREE_HAL_POOL_ACQUIRE_OK_FRESH;
  }

  if (elements_allocated) {
    iree_allocator_free(pool->host_allocator, elements);
  }
  call->status = status;
}

static iree_status_t iree_hal_passthrough_pool_acquire_reservations(
    iree_hal_pool_t* base_pool, iree_host_size_t request_count,
    const iree_hal_pool_reservation_request_t* requests,
    const iree_async_frontier_t* requester_frontier,
    iree_hal_pool_reserve_flags_t flags,
    iree_hal_pool_reservation_t* out_reservations,
    iree_hal_pool_acquire_info_t* out_infos,
    iree_hal_pool_acquire_result_t* out_result) {
  iree_hal_passthrough_pool_t* pool = (iree_hal_passthrough_pool_t*)base_pool;
  if (!pool->owned_enabled) {
    return iree_make_status(
        IREE_STATUS_FAILED_PRECONDITION,
        "import-only pool cannot acquire provider-owned backing");
  }
  for (iree_host_size_t i = 0; i < request_count; ++i) {
    IREE_RETURN_IF_ERROR(iree_hal_passthrough_pool_validate_reservation_request(
        pool, &requests[i]));
  }
  const iree_status_t failure = (iree_status_t)iree_atomic_load(
      &pool->failure_status, iree_memory_order_acquire);
  if (!iree_status_is_ok(failure)) {
    return iree_status_clone(failure);
  }
  // Every reservation requires new backing. Defer the whole transaction before
  // allocating either native storage or transaction metadata.
  if (iree_any_bit_set(flags, IREE_HAL_POOL_RESERVE_FLAG_DISALLOW_GROWTH)) {
    iree_atomic_fetch_add(&pool->exhausted_count, 1, iree_memory_order_relaxed);
    for (iree_host_size_t i = 0; i < request_count; ++i) {
      out_infos[i] = (iree_hal_pool_acquire_info_t){
          .result = IREE_HAL_POOL_ACQUIRE_EXHAUSTED,
          .flags = IREE_HAL_POOL_ACQUIRE_FLAG_GROWTH_REQUIRED,
      };
    }
    *out_result = IREE_HAL_POOL_ACQUIRE_EXHAUSTED;
    return iree_ok_status();
  }

  iree_hal_passthrough_pool_acquire_call_t call = {
      .pool = pool,
      .request_count = request_count,
      .requests = requests,
      .out_reservations = out_reservations,
      .out_infos = out_infos,
      .out_result = out_result,
  };
  iree_hal_memory_maintenance_call(
      pool->maintenance, iree_hal_passthrough_pool_acquire_on_owner, &call);
  return call.status;
}

static void iree_hal_passthrough_pool_release_one_reservation(
    iree_hal_pool_t* base_pool, const iree_hal_pool_reservation_t* reservation,
    const iree_async_frontier_t* death_frontier) {
  (void)base_pool;

  iree_hal_passthrough_pool_reservation_state_t* reservation_state =
      (iree_hal_passthrough_pool_reservation_state_t*)(uintptr_t)
          reservation->block_handle;
  iree_hal_passthrough_pool_reservation_state_release_reservation(
      reservation_state, death_frontier);
}

static void iree_hal_passthrough_pool_release_reservations(
    iree_hal_pool_t* base_pool, iree_host_size_t reservation_count,
    const iree_hal_pool_reservation_t* reservations,
    const iree_async_frontier_t* death_frontier) {
  iree_hal_passthrough_pool_t* pool = (iree_hal_passthrough_pool_t*)base_pool;
  for (iree_host_size_t i = 0; i < reservation_count; ++i) {
    iree_hal_passthrough_pool_release_one_reservation(
        base_pool, &reservations[i], death_frontier);
  }
  iree_async_notification_signal_if_observed(pool->base.notification,
                                             INT32_MAX);
}

//===----------------------------------------------------------------------===//
// Wrap / Query / Trim / Notification
//===----------------------------------------------------------------------===//

static iree_status_t iree_hal_passthrough_pool_materialize_reservations(
    iree_hal_pool_t* base_pool, iree_host_size_t reservation_count,
    const iree_hal_pool_reservation_request_t* requests,
    const iree_hal_pool_reservation_t* reservations,
    iree_hal_pool_materialize_flags_t flags, iree_hal_buffer_t** out_buffers) {
  iree_hal_passthrough_pool_t* pool = (iree_hal_passthrough_pool_t*)base_pool;

  for (iree_host_size_t i = 0; i < reservation_count; ++i) {
    if (reservations[i].byte_length < requests[i].allocation_size) {
      return iree_make_status(
          IREE_STATUS_INVALID_ARGUMENT,
          "reservation %" PRIhsz " has %" PRIdsz
          " bytes but its allocation request requires %" PRIdsz,
          i, reservations[i].byte_length, requests[i].allocation_size);
    }
    if (!reservations[i].block_handle) {
      return iree_make_status(
          IREE_STATUS_INVALID_ARGUMENT,
          "reservation %" PRIhsz " has no pass-through state", i);
    }
  }

  const bool transfer_ownership = iree_all_bits_set(
      flags, IREE_HAL_POOL_MATERIALIZE_FLAG_TRANSFER_RESERVATION_OWNERSHIP);
  iree_hal_passthrough_pool_materialize_state_t* state = NULL;
  iree_hal_buffer_t*
      inline_buffers[IREE_HAL_PASSTHROUGH_POOL_INLINE_TRANSACTION_CAPACITY] = {
          0};
  iree_hal_buffer_t** staged_buffers = inline_buffers;
  bool staged_buffers_allocated = false;
  iree_status_t status = iree_ok_status();
  if (transfer_ownership) {
    if (reservation_count > INT32_MAX) {
      return iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                              "materialization count exceeds INT32_MAX");
    }
    iree_host_size_t state_size = 0;
    if (!iree_host_size_checked_mul_add(
            reservation_count,
            sizeof(iree_hal_passthrough_pool_materialize_element_t),
            sizeof(*state), &state_size)) {
      return iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                              "materialization state size overflow");
    }
    status =
        iree_allocator_malloc(pool->host_allocator, state_size, (void**)&state);
    if (!iree_status_is_ok(status)) {
      return status;
    }
    memset(state, 0, state_size);
    state->host_allocator = pool->host_allocator;
    iree_atomic_store(&state->reference_count, (int32_t)reservation_count,
                      iree_memory_order_relaxed);
    for (iree_host_size_t i = 0; i < reservation_count; ++i) {
      state->elements[i].state = state;
      state->elements[i].reservation_state =
          (iree_hal_passthrough_pool_reservation_state_t*)(uintptr_t)
              reservations[i]
                  .block_handle;
    }
  } else if (reservation_count > IREE_ARRAYSIZE(inline_buffers)) {
    status = iree_allocator_malloc_array(
        pool->host_allocator, reservation_count, sizeof(*staged_buffers),
        (void**)&staged_buffers);
    if (!iree_status_is_ok(status)) {
      return status;
    }
    staged_buffers_allocated = true;
    memset(staged_buffers, 0, reservation_count * sizeof(*staged_buffers));
  }

  iree_host_size_t materialized_count = 0;
  while (materialized_count < reservation_count && iree_status_is_ok(status)) {
    iree_hal_passthrough_pool_reservation_state_t* reservation_state =
        (iree_hal_passthrough_pool_reservation_state_t*)(uintptr_t)
            reservations[materialized_count]
                .block_handle;
    iree_atomic_fetch_add(&reservation_state->reference_count, 1,
                          iree_memory_order_acq_rel);
    iree_hal_buffer_release_callback_t release_callback = {
        .fn = iree_hal_passthrough_pool_borrowed_view_release,
        .user_data = reservation_state,
    };
    iree_hal_buffer_t** staged_buffer = &staged_buffers[materialized_count];
    if (state) {
      release_callback.fn = iree_hal_passthrough_pool_owned_buffer_release;
      release_callback.user_data = &state->elements[materialized_count];
      staged_buffer = &state->elements[materialized_count].buffer;
    }
    iree_hal_memory_access_t allowed_access = IREE_HAL_MEMORY_ACCESS_ALL;
    iree_hal_buffer_usage_t allowed_usage =
        pool->slab_properties.supported_usage;
    if (base_pool->memory_contract) {
      allowed_access = base_pool->memory_contract->allowed_access;
      allowed_usage = base_pool->memory_contract->allowed_usage;
    }
    status = iree_hal_slab_provider_wrap_buffer(
        pool->slab_provider, &reservation_state->allocation.slab,
        reservations[materialized_count].offset,
        reservations[materialized_count].byte_length, allowed_access,
        allowed_usage, release_callback, staged_buffer);
    if (iree_status_is_ok(status)) {
      (*staged_buffer)->memory.contract = base_pool->memory_contract;
      (*staged_buffer)->memory.backing =
          &reservation_state->allocation.buffer_backing.facts;
      (*staged_buffer)->memory.offset = reservations[materialized_count].offset;
      ++materialized_count;
    } else {
      iree_hal_passthrough_pool_reservation_state_release_reference(
          reservation_state);
    }
  }

  if (iree_status_is_ok(status)) {
    if (state) {
      state->ownership_committed = true;
    }
    for (iree_host_size_t i = 0; i < reservation_count; ++i) {
      out_buffers[i] = state ? state->elements[i].buffer : staged_buffers[i];
    }
  } else {
    if (state) {
      iree_atomic_store(&state->reference_count, (int32_t)materialized_count,
                        iree_memory_order_relaxed);
    }
    for (iree_host_size_t i = 0; i < materialized_count; ++i) {
      iree_hal_buffer_release(state ? state->elements[i].buffer
                                    : staged_buffers[i]);
    }
    if (state && materialized_count == 0) {
      iree_allocator_free(pool->host_allocator, state);
    }
  }
  if (staged_buffers_allocated) {
    iree_allocator_free(pool->host_allocator, staged_buffers);
  }
  return status;
}

static void iree_hal_passthrough_pool_query_capabilities(
    const iree_hal_pool_t* base_pool,
    iree_hal_pool_capabilities_t* out_capabilities) {
  const iree_hal_passthrough_pool_t* pool =
      (const iree_hal_passthrough_pool_t*)base_pool;
  out_capabilities->memory_type = pool->slab_properties.memory_type;
  out_capabilities->allowed_access = IREE_HAL_MEMORY_ACCESS_ALL;
  out_capabilities->supported_usage = pool->slab_properties.supported_usage;
  out_capabilities->queue_family_affinity =
      pool->slab_properties.queue_family_affinity;
  out_capabilities->atomic_operations = pool->slab_properties.atomic_operations;
  out_capabilities->min_allocation_size = 0;
  out_capabilities->max_allocation_size = 0;
  out_capabilities->max_allocation_alignment =
      pool->slab_properties.max_allocation_alignment;
  out_capabilities->maintenance_alignment =
      pool->slab_properties.maintenance_alignment;
  if (base_pool->memory_contract) {
    out_capabilities->allowed_access =
        base_pool->memory_contract->allowed_access;
    out_capabilities->supported_usage =
        base_pool->memory_contract->allowed_usage;
  }
}

static iree_status_t iree_hal_passthrough_pool_validate_asan(
    const iree_hal_pool_t* base_pool,
    const iree_hal_asan_pool_options_t* options) {
  const iree_hal_passthrough_pool_t* pool =
      (const iree_hal_passthrough_pool_t*)base_pool;
  return iree_hal_slab_provider_validate_asan_options(pool->slab_provider,
                                                      options);
}

static void iree_hal_passthrough_pool_query_stats(
    const iree_hal_pool_t* base_pool, iree_hal_pool_stats_t* out_stats) {
  const iree_hal_passthrough_pool_t* pool =
      (const iree_hal_passthrough_pool_t*)base_pool;
  out_stats->bytes_reserved = (iree_device_size_t)iree_atomic_load(
      &pool->bytes_reserved, iree_memory_order_relaxed);
  out_stats->bytes_free = 0;
  out_stats->bytes_committed = (iree_device_size_t)iree_atomic_load(
      &pool->bytes_committed, iree_memory_order_relaxed);
  out_stats->budget_limit = 0;
  out_stats->reservation_count = (uint32_t)iree_atomic_load(
      &pool->reservation_count, iree_memory_order_relaxed);
  out_stats->slab_count =
      (uint32_t)iree_atomic_load(&pool->slab_count, iree_memory_order_relaxed);
  out_stats->reserve_count = (uint64_t)iree_atomic_load(
      &pool->reserve_count, iree_memory_order_relaxed);
  out_stats->release_count = (uint64_t)iree_atomic_load(
      &pool->release_count, iree_memory_order_relaxed);
  out_stats->reuse_count = 0;
  out_stats->reuse_miss_count = 0;
  out_stats->fresh_count = out_stats->reserve_count;
  out_stats->exhausted_count = (uint64_t)iree_atomic_load(
      &pool->exhausted_count, iree_memory_order_relaxed);
  out_stats->over_budget_count = 0;
  out_stats->wait_count = 0;
}

static void iree_hal_passthrough_pool_trim(
    iree_hal_pool_t* base_pool, iree_hal_pool_trim_flags_t flags,
    iree_device_size_t min_bytes_to_keep) {
  iree_hal_passthrough_pool_t* pool = (iree_hal_passthrough_pool_t*)base_pool;
  // Every returned slab already has retirement scheduled. This pool retains
  // no idle backing and does not wait on the shared maintenance executor.
  (void)min_bytes_to_keep;
  iree_hal_slab_provider_trim(pool->slab_provider, flags);
}

//===----------------------------------------------------------------------===//
// Vtable
//===----------------------------------------------------------------------===//

static const iree_hal_pool_vtable_t iree_hal_passthrough_pool_vtable = {
    .destroy = iree_hal_passthrough_pool_destroy,
    .acquire_reservations = iree_hal_passthrough_pool_acquire_reservations,
    .release_reservations = iree_hal_passthrough_pool_release_reservations,
    .materialize_reservations =
        iree_hal_passthrough_pool_materialize_reservations,
    .import_buffer = iree_hal_passthrough_pool_import_buffer,
    .query_capabilities = iree_hal_passthrough_pool_query_capabilities,
    .validate_asan = iree_hal_passthrough_pool_validate_asan,
    .query_stats = iree_hal_passthrough_pool_query_stats,
    .trim = iree_hal_passthrough_pool_trim,
    .advise_asan_reservations =
        iree_hal_passthrough_pool_advise_asan_reservations,
    .query_import_requirements =
        iree_hal_passthrough_pool_query_import_requirements,
};
