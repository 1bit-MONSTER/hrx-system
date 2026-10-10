// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef IREE_HAL_MEMORY_MAINTENANCE_H_
#define IREE_HAL_MEMORY_MAINTENANCE_H_

#include "iree/base/api.h"
#include "iree/base/threading/mutex.h"
#include "iree/hal/resource.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

typedef struct iree_hal_memory_maintenance_t iree_hal_memory_maintenance_t;
typedef struct iree_hal_memory_maintenance_entry_t
    iree_hal_memory_maintenance_entry_t;

// One-shot cold memory work in caller-owned storage. The entry remains alive
// until its callback begins. The callback may free it; the owner does not
// access it afterward. An entry cannot be enqueued again while it is already
// pending.
typedef struct iree_hal_memory_maintenance_entry_t {
  // Next entry while owned by the maintenance queue.
  iree_hal_memory_maintenance_entry_t* next;
  // Native preparation or cleanup to run outside the queue's metadata lock.
  void(IREE_API_PTR* fn)(iree_hal_memory_maintenance_entry_t* entry);
} iree_hal_memory_maintenance_entry_t;

// Retains the placement-local owner of cold memory work. Pools sharing a native
// memory domain share this owner instead of creating a worker per pool.
void iree_hal_memory_maintenance_retain(
    iree_hal_memory_maintenance_t* maintenance);

// Releases the owner. Final release joins its work and must run outside that
// owner's executor. All producers must have finished before final release.
void iree_hal_memory_maintenance_release(
    iree_hal_memory_maintenance_t* maintenance);

// Publishes prepared work without allocation. Queue links are protected by a
// short metadata lock; wakeup runs outside it. Thread-backed owners run work on
// their captured executor. Inline owners may run and free |entry| before this
// call returns and are reserved for native APIs whose execution context cannot
// block waiting for another executor.
void iree_hal_memory_maintenance_enqueue(
    iree_hal_memory_maintenance_t* maintenance,
    iree_hal_memory_maintenance_entry_t* entry);

// Runs cold preparation on its captured owner and joins the call before
// returning. Calls already executing on that owner run inline, allowing an
// allocator to prepare storage through a parent sharing the same owner.
//
// This may block on native preparation and queued maintenance. It is not a
// capacity wait and must not run on allocation fast paths, under allocator
// metadata locks, or on proactor poll callbacks. The callback must not wait for
// execution progress or for another allocation to release capacity.
void iree_hal_memory_maintenance_call(
    iree_hal_memory_maintenance_t* maintenance,
    void(IREE_API_PTR* fn)(void* user_data), void* user_data);

//===----------------------------------------------------------------------===//
// Implementer interface
//===----------------------------------------------------------------------===//

typedef struct iree_hal_memory_maintenance_vtable_t {
  // Joins the executor before deinitializing and freeing the owner.
  void(IREE_API_PTR* destroy)(iree_hal_memory_maintenance_t* maintenance);
  // Publishes a pending wake to the captured executor. May race a drain.
  void(IREE_API_PTR* wake)(iree_hal_memory_maintenance_t* maintenance);
} iree_hal_memory_maintenance_vtable_t;
IREE_HAL_ASSERT_VTABLE_LAYOUT(iree_hal_memory_maintenance_vtable_t);

// Shared queue prefix embedded in an executor-specific owner. The executor is
// single-consumer; callbacks run in enqueue order without the metadata lock.
struct iree_hal_memory_maintenance_t {
  // Reference-counted executor ownership.
  iree_hal_resource_t resource;
  // Protects only the pending entry links.
  iree_slim_mutex_t mutex;
  // First pending entry, or NULL when the queue is empty.
  iree_hal_memory_maintenance_entry_t* head;
  // Last pending entry, or NULL when the queue is empty.
  iree_hal_memory_maintenance_entry_t* tail;
};

void iree_hal_memory_maintenance_initialize(
    const iree_hal_memory_maintenance_vtable_t* vtable,
    iree_hal_memory_maintenance_t* out_maintenance);
void iree_hal_memory_maintenance_deinitialize(
    iree_hal_memory_maintenance_t* maintenance);

// Runs at most one entry on the selected cold owner. Returns whether work ran.
// The executor's wake protocol must cover publication racing an empty result.
bool iree_hal_memory_maintenance_run_one(
    iree_hal_memory_maintenance_t* maintenance);

// Returns whether an entry is queued. Used by the executor's sleep predicate.
bool iree_hal_memory_maintenance_has_work(
    iree_hal_memory_maintenance_t* maintenance);

// Creates a serialized owner that drains work on an enqueueing thread.
//
// This is intended for synchronous, execution-context-affine native APIs such
// as browser WebGPU. It creates no thread and never blocks waiting for another
// executor. Callbacks from several producers remain single-consumer, but the
// winning producer supplies the execution context. Every producer must
// therefore already satisfy the native API's thread-affinity contract.
iree_status_t iree_hal_memory_maintenance_inline_create(
    iree_allocator_t host_allocator,
    iree_hal_memory_maintenance_t** out_maintenance);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // IREE_HAL_MEMORY_MAINTENANCE_H_
