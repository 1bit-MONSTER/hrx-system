// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/hal/memory/maintenance_thread.h"

#include "iree/base/threading/notification.h"
#include "iree/base/threading/thread.h"

typedef struct iree_hal_memory_maintenance_thread_t {
  // Shared intrusive work queue.
  iree_hal_memory_maintenance_t base;
  // Allocator for this owner and its thread.
  iree_allocator_t host_allocator;
  // Wake channel for new work and final shutdown.
  iree_notification_t notification;
  // Set after every producer has released its ownership.
  iree_atomic_int32_t stopping;
  // Owned thread, or NULL during failed construction.
  iree_thread_t* thread;
} iree_hal_memory_maintenance_thread_t;

static bool iree_hal_memory_maintenance_thread_is_ready(void* user_data) {
  iree_hal_memory_maintenance_thread_t* maintenance =
      (iree_hal_memory_maintenance_thread_t*)user_data;
  return iree_atomic_load(&maintenance->stopping, iree_memory_order_acquire) ||
         iree_hal_memory_maintenance_has_work(&maintenance->base);
}

static int iree_hal_memory_maintenance_thread_main(void* user_data) {
  iree_hal_memory_maintenance_thread_t* maintenance =
      (iree_hal_memory_maintenance_thread_t*)user_data;
  while (true) {
    if (iree_hal_memory_maintenance_run_one(&maintenance->base)) {
      continue;
    }
    if (iree_atomic_load(&maintenance->stopping, iree_memory_order_acquire)) {
      break;
    }
    iree_notification_await(&maintenance->notification,
                            iree_hal_memory_maintenance_thread_is_ready,
                            maintenance, iree_infinite_timeout());
  }
  return 0;
}

static void iree_hal_memory_maintenance_thread_wake(
    iree_hal_memory_maintenance_t* base_maintenance) {
  iree_hal_memory_maintenance_thread_t* maintenance =
      (iree_hal_memory_maintenance_thread_t*)base_maintenance;
  iree_notification_post(&maintenance->notification, IREE_ALL_WAITERS);
}

static void iree_hal_memory_maintenance_thread_destroy(
    iree_hal_memory_maintenance_t* base_maintenance) {
  IREE_TRACE_ZONE_BEGIN(z0);
  iree_hal_memory_maintenance_thread_t* maintenance =
      (iree_hal_memory_maintenance_thread_t*)base_maintenance;
  iree_atomic_store(&maintenance->stopping, 1, iree_memory_order_release);
  iree_hal_memory_maintenance_thread_wake(base_maintenance);
  iree_thread_release(maintenance->thread);
  iree_notification_deinitialize(&maintenance->notification);
  iree_hal_memory_maintenance_deinitialize(base_maintenance);
  iree_allocator_free(maintenance->host_allocator, maintenance);
  IREE_TRACE_ZONE_END(z0);
}

static const iree_hal_memory_maintenance_vtable_t
    iree_hal_memory_maintenance_thread_vtable = {
        .destroy = iree_hal_memory_maintenance_thread_destroy,
        .wake = iree_hal_memory_maintenance_thread_wake,
};

iree_status_t iree_hal_memory_maintenance_thread_create(
    iree_thread_affinity_t affinity, iree_allocator_t host_allocator,
    iree_hal_memory_maintenance_t** out_maintenance) {
  IREE_TRACE_ZONE_BEGIN(z0);
  *out_maintenance = NULL;
  iree_hal_memory_maintenance_thread_t* maintenance = NULL;
  IREE_RETURN_AND_END_ZONE_IF_ERROR(
      z0, iree_allocator_malloc(host_allocator, sizeof(*maintenance),
                                (void**)&maintenance));
  iree_hal_memory_maintenance_initialize(
      &iree_hal_memory_maintenance_thread_vtable, &maintenance->base);
  maintenance->host_allocator = host_allocator;
  iree_notification_initialize(&maintenance->notification);
  const iree_thread_create_params_t params = {
      .name = IREE_SV("hal-memory"),
      .initial_affinity = affinity,
  };
  iree_status_t status =
      iree_thread_create(iree_hal_memory_maintenance_thread_main, maintenance,
                         params, host_allocator, &maintenance->thread);
  if (iree_status_is_ok(status)) {
    *out_maintenance = &maintenance->base;
  } else {
    iree_hal_memory_maintenance_release(&maintenance->base);
  }
  IREE_TRACE_ZONE_END(z0);
  return status;
}
