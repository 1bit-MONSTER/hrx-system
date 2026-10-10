// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/hal/memory/maintenance.h"

#include "iree/base/threading/notification.h"

// Identifies the current executor for reentrant cold preparation calls.
static IREE_THREAD_LOCAL iree_hal_memory_maintenance_t*
    iree_hal_memory_maintenance_current;

typedef struct iree_hal_memory_maintenance_call_t {
  // Stack-owned entry joined before the calling thread returns.
  iree_hal_memory_maintenance_entry_t entry;
  // Cold preparation callback.
  void(IREE_API_PTR* fn)(void* user_data);
  // Borrowed callback state, valid until this call returns.
  void* user_data;
  // Protects completion through the callback's final notification access.
  iree_slim_mutex_t mutex;
  // Wakes the calling thread when preparation finishes.
  iree_notification_t notification;
  // True after the callback has finished accessing user_data.
  bool complete;
} iree_hal_memory_maintenance_call_t;

static bool iree_hal_memory_maintenance_call_is_complete(void* user_data) {
  iree_hal_memory_maintenance_call_t* call = user_data;
  iree_slim_mutex_lock(&call->mutex);
  const bool complete = call->complete;
  iree_slim_mutex_unlock(&call->mutex);
  return complete;
}

static void iree_hal_memory_maintenance_call_run(
    iree_hal_memory_maintenance_entry_t* entry) {
  iree_hal_memory_maintenance_call_t* call =
      (iree_hal_memory_maintenance_call_t*)entry;
  call->fn(call->user_data);
  iree_slim_mutex_lock(&call->mutex);
  call->complete = true;
  iree_notification_post(&call->notification, IREE_ALL_WAITERS);
  iree_slim_mutex_unlock(&call->mutex);
}

void iree_hal_memory_maintenance_call(
    iree_hal_memory_maintenance_t* maintenance,
    void(IREE_API_PTR* fn)(void* user_data), void* user_data) {
  IREE_TRACE_ZONE_BEGIN(z0);
  if (iree_hal_memory_maintenance_current == maintenance) {
    fn(user_data);
    IREE_TRACE_ZONE_END(z0);
    return;
  }
  iree_hal_memory_maintenance_call_t call = {
      .entry = {.fn = iree_hal_memory_maintenance_call_run},
      .fn = fn,
      .user_data = user_data,
  };
  iree_slim_mutex_initialize(&call.mutex);
  iree_notification_initialize(&call.notification);
  iree_hal_memory_maintenance_enqueue(maintenance, &call.entry);
  iree_notification_await(&call.notification,
                          iree_hal_memory_maintenance_call_is_complete, &call,
                          iree_infinite_timeout());
  iree_notification_deinitialize(&call.notification);
  iree_slim_mutex_deinitialize(&call.mutex);
  IREE_TRACE_ZONE_END(z0);
}

void iree_hal_memory_maintenance_initialize(
    const iree_hal_memory_maintenance_vtable_t* vtable,
    iree_hal_memory_maintenance_t* out_maintenance) {
  memset(out_maintenance, 0, sizeof(*out_maintenance));
  iree_hal_resource_initialize(vtable, &out_maintenance->resource);
  iree_slim_mutex_initialize(&out_maintenance->mutex);
}

void iree_hal_memory_maintenance_deinitialize(
    iree_hal_memory_maintenance_t* maintenance) {
  IREE_ASSERT(!maintenance->head, "maintenance work must be joined");
  iree_slim_mutex_deinitialize(&maintenance->mutex);
}

void iree_hal_memory_maintenance_retain(
    iree_hal_memory_maintenance_t* maintenance) {
  iree_hal_resource_retain(maintenance);
}

void iree_hal_memory_maintenance_release(
    iree_hal_memory_maintenance_t* maintenance) {
  iree_hal_resource_release(maintenance);
}

void iree_hal_memory_maintenance_enqueue(
    iree_hal_memory_maintenance_t* maintenance,
    iree_hal_memory_maintenance_entry_t* entry) {
  IREE_TRACE_ZONE_BEGIN(z0);
  entry->next = NULL;
  iree_slim_mutex_lock(&maintenance->mutex);
  const bool needs_wake = maintenance->head == NULL;
  if (maintenance->tail) {
    maintenance->tail->next = entry;
  } else {
    maintenance->head = entry;
  }
  maintenance->tail = entry;
  iree_slim_mutex_unlock(&maintenance->mutex);
  if (needs_wake) {
    const iree_hal_memory_maintenance_vtable_t* vtable =
        (const iree_hal_memory_maintenance_vtable_t*)
            maintenance->resource.vtable;
    vtable->wake(maintenance);
  }
  IREE_TRACE_ZONE_END(z0);
}

bool iree_hal_memory_maintenance_has_work(
    iree_hal_memory_maintenance_t* maintenance) {
  iree_slim_mutex_lock(&maintenance->mutex);
  const bool has_work = maintenance->head != NULL;
  iree_slim_mutex_unlock(&maintenance->mutex);
  return has_work;
}

bool iree_hal_memory_maintenance_run_one(
    iree_hal_memory_maintenance_t* maintenance) {
  iree_slim_mutex_lock(&maintenance->mutex);
  iree_hal_memory_maintenance_entry_t* entry = maintenance->head;
  if (entry) {
    maintenance->head = entry->next;
    if (!maintenance->head) {
      maintenance->tail = NULL;
    }
  }
  iree_slim_mutex_unlock(&maintenance->mutex);
  if (!entry) {
    return false;
  }
  IREE_TRACE_ZONE_BEGIN(z0);
  iree_hal_memory_maintenance_t* previous = iree_hal_memory_maintenance_current;
  iree_hal_memory_maintenance_current = maintenance;
  entry->fn(entry);
  iree_hal_memory_maintenance_current = previous;
  IREE_TRACE_ZONE_END(z0);
  return true;
}

//===----------------------------------------------------------------------===//
// Inline owner
//===----------------------------------------------------------------------===//

typedef struct iree_hal_memory_maintenance_inline_t {
  // Shared intrusive work queue.
  iree_hal_memory_maintenance_t base;
  // Allocator for this owner.
  iree_allocator_t host_allocator;
  // Selects one enqueueing thread to drain at a time.
  iree_atomic_int32_t draining;
} iree_hal_memory_maintenance_inline_t;

static void iree_hal_memory_maintenance_inline_wake(
    iree_hal_memory_maintenance_t* base_maintenance) {
  iree_hal_memory_maintenance_inline_t* maintenance =
      (iree_hal_memory_maintenance_inline_t*)base_maintenance;
  if (iree_atomic_exchange(&maintenance->draining, 1,
                           iree_memory_order_acq_rel)) {
    return;
  }
  while (true) {
    while (iree_hal_memory_maintenance_run_one(base_maintenance)) {
    }
    iree_atomic_store(&maintenance->draining, 0, iree_memory_order_release);
    if (!iree_hal_memory_maintenance_has_work(base_maintenance) ||
        iree_atomic_exchange(&maintenance->draining, 1,
                             iree_memory_order_acq_rel)) {
      break;
    }
  }
}

static void iree_hal_memory_maintenance_inline_destroy(
    iree_hal_memory_maintenance_t* base_maintenance) {
  iree_hal_memory_maintenance_inline_t* maintenance =
      (iree_hal_memory_maintenance_inline_t*)base_maintenance;
  IREE_ASSERT(
      !iree_atomic_load(&maintenance->draining, iree_memory_order_acquire));
  iree_allocator_t host_allocator = maintenance->host_allocator;
  iree_hal_memory_maintenance_deinitialize(base_maintenance);
  iree_allocator_free(host_allocator, maintenance);
}

static const iree_hal_memory_maintenance_vtable_t
    iree_hal_memory_maintenance_inline_vtable = {
        .destroy = iree_hal_memory_maintenance_inline_destroy,
        .wake = iree_hal_memory_maintenance_inline_wake,
};

iree_status_t iree_hal_memory_maintenance_inline_create(
    iree_allocator_t host_allocator,
    iree_hal_memory_maintenance_t** out_maintenance) {
  IREE_ASSERT_ARGUMENT(out_maintenance);
  *out_maintenance = NULL;
  iree_hal_memory_maintenance_inline_t* maintenance = NULL;
  IREE_RETURN_IF_ERROR(iree_allocator_malloc(
      host_allocator, sizeof(*maintenance), (void**)&maintenance));
  iree_hal_memory_maintenance_initialize(
      &iree_hal_memory_maintenance_inline_vtable, &maintenance->base);
  maintenance->host_allocator = host_allocator;
  maintenance->draining = IREE_ATOMIC_VAR_INIT(0);
  *out_maintenance = &maintenance->base;
  return iree_ok_status();
}
