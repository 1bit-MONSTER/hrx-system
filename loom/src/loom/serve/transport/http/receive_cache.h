// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_SERVE_TRANSPORT_HTTP_RECEIVE_CACHE_H_
#define LOOM_SERVE_TRANSPORT_HTTP_RECEIVE_CACHE_H_

#include "iree/async/buffer_pool.h"
#include "iree/async/util/proactor_pool.h"

#ifdef __cplusplus
extern "C" {
#endif

// Idle registered receive pools for one listener's proactor and buffer shape.
// Each acquired slot is exclusive to one carrier. Slots are created on demand
// and recycled after I/O retirement; the listener's admission budget bounds
// peak storage. No slab or registration is created on a cache hit.
typedef struct loom_serve_http_receive_cache_t loom_serve_http_receive_cache_t;
typedef struct loom_serve_http_receive_slot_t loom_serve_http_receive_slot_t;

// Retains the entry and creates an empty cache. Buffer dimensions have already
// been validated by listener configuration. Acquired slots retain the cache,
// so accepted connections can outlive both the listener and its cache
// reference.
iree_status_t loom_serve_http_receive_cache_create(
    iree_async_proactor_pool_entry_t* entry, iree_host_size_t buffer_size,
    iree_host_size_t buffer_count, iree_allocator_t host_allocator,
    loom_serve_http_receive_cache_t** out_cache);

// Retains the cache and its registration owner. NULL is accepted.
void loom_serve_http_receive_cache_retain(
    loom_serve_http_receive_cache_t* cache);

// Drops the owner's reference. Final release deregisters idle slots before
// releasing their proactor entry. NULL is accepted.
void loom_serve_http_receive_cache_release(
    loom_serve_http_receive_cache_t* cache);

// Acquires an exclusive slot, growing the cache only if no idle slot exists.
// On failure no ownership transfers and out_slot is NULL. Concurrent acquire
// and recycle calls are supported; registration never holds the cache mutex.
iree_status_t loom_serve_http_receive_cache_acquire(
    loom_serve_http_receive_cache_t* cache,
    loom_serve_http_receive_slot_t** out_slot);

// Borrows the registered buffer pool until slot recycle. The carrier retains
// its own reference; that carrier and every receive lease must retire before
// the slot can be recycled to another carrier.
iree_async_buffer_pool_t* loom_serve_http_receive_slot_buffers(
    loom_serve_http_receive_slot_t* slot);

// Returns the exclusive slot after its carrier and receive leases retire.
// The caller relinquishes all access. NULL is accepted.
void loom_serve_http_receive_slot_recycle(loom_serve_http_receive_slot_t* slot);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_SERVE_TRANSPORT_HTTP_RECEIVE_CACHE_H_
