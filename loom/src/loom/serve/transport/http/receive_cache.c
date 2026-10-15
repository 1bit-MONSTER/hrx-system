// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/serve/transport/http/receive_cache.h"

#include "iree/async/slab.h"
#include "iree/base/threading/mutex.h"

struct loom_serve_http_receive_slot_t {
  // Retained while acquired; borrowed while on the cache's idle list.
  loom_serve_http_receive_cache_t* cache;
  // Next idle slot, protected by the cache mutex; NULL while acquired.
  struct loom_serve_http_receive_slot_t* next;
  // Owned registered receive pool, exclusive to one carrier at a time.
  iree_async_buffer_pool_t* buffers;
};

struct loom_serve_http_receive_cache_t {
  // Listener ownership plus one reference per acquired slot.
  iree_atomic_ref_count_t ref_count;
  // Allocator owning the cache, slots, and buffer-pool metadata.
  iree_allocator_t host_allocator;
  // Retained registration/progress owner, released after all regions retire.
  iree_async_proactor_pool_entry_t* entry;
  // Immutable per-connection receive shape and default NUMA placement.
  iree_async_slab_options_t options;
  // Protects only the idle list; allocation and deregistration run outside it.
  iree_slim_mutex_t mutex;
  // Reusable exclusive receive pools; borrowed slots retain the cache instead.
  loom_serve_http_receive_slot_t* idle;
};

iree_status_t loom_serve_http_receive_cache_create(
    iree_async_proactor_pool_entry_t* entry, iree_host_size_t buffer_size,
    iree_host_size_t buffer_count, iree_allocator_t host_allocator,
    loom_serve_http_receive_cache_t** out_cache) {
  IREE_TRACE_ZONE_BEGIN(z0);
  *out_cache = NULL;
  loom_serve_http_receive_cache_t* cache = NULL;
  IREE_RETURN_AND_END_ZONE_IF_ERROR(
      z0,
      iree_allocator_malloc(host_allocator, sizeof(*cache), (void**)&cache));
  iree_atomic_ref_count_init(&cache->ref_count);
  cache->host_allocator = host_allocator;
  cache->entry = entry;
  iree_async_proactor_pool_entry_retain(entry);
  cache->options.buffer_size = buffer_size;
  cache->options.buffer_count = buffer_count;
  iree_slim_mutex_initialize(&cache->mutex);
  *out_cache = cache;
  IREE_TRACE_ZONE_END(z0);
  return iree_ok_status();
}

void loom_serve_http_receive_cache_retain(
    loom_serve_http_receive_cache_t* cache) {
  if (cache) {
    iree_atomic_ref_count_inc(&cache->ref_count);
  }
}

void loom_serve_http_receive_cache_release(
    loom_serve_http_receive_cache_t* cache) {
  if (!cache) {
    return;
  }
  if (iree_atomic_ref_count_dec(&cache->ref_count) != 1) {
    return;
  }
  IREE_TRACE_ZONE_BEGIN(z0);
  // Every acquired slot holds a reference, so final release owns the entire
  // list and no native receive or consumer lease can still use its storage.
  loom_serve_http_receive_slot_t* slot = cache->idle;
  while (slot) {
    loom_serve_http_receive_slot_t* next = slot->next;
    iree_async_buffer_pool_release(slot->buffers);
    iree_allocator_free(cache->host_allocator, slot);
    slot = next;
  }
  iree_async_proactor_pool_entry_release(cache->entry);
  iree_slim_mutex_deinitialize(&cache->mutex);
  iree_allocator_free(cache->host_allocator, cache);
  IREE_TRACE_ZONE_END(z0);
}

static iree_status_t loom_serve_http_receive_cache_grow(
    loom_serve_http_receive_cache_t* cache,
    loom_serve_http_receive_slot_t** out_slot) {
  IREE_TRACE_ZONE_BEGIN(z0);
  loom_serve_http_receive_slot_t* slot = NULL;
  IREE_RETURN_AND_END_ZONE_IF_ERROR(
      z0, iree_allocator_malloc(cache->host_allocator, sizeof(*slot),
                                (void**)&slot));
  slot->cache = cache;
  iree_async_slab_t* slab = NULL;
  iree_async_region_t* region = NULL;
  iree_status_t status =
      iree_async_slab_create(cache->options, cache->host_allocator, &slab);
  if (iree_status_is_ok(status)) {
    status = iree_async_proactor_register_slab(
        iree_async_proactor_pool_entry_proactor(cache->entry), slab,
        IREE_ASYNC_BUFFER_ACCESS_FLAG_WRITE, &region);
  }
  if (iree_status_is_ok(status)) {
    status = iree_async_buffer_pool_create(region, cache->host_allocator,
                                           &slot->buffers);
  }
  iree_async_region_release(region);
  iree_async_slab_release(slab);
  if (iree_status_is_ok(status)) {
    *out_slot = slot;
  } else {
    iree_allocator_free(cache->host_allocator, slot);
  }
  IREE_TRACE_ZONE_END(z0);
  return status;
}

iree_status_t loom_serve_http_receive_cache_acquire(
    loom_serve_http_receive_cache_t* cache,
    loom_serve_http_receive_slot_t** out_slot) {
  IREE_TRACE_ZONE_BEGIN(z0);
  *out_slot = NULL;
  iree_slim_mutex_lock(&cache->mutex);
  loom_serve_http_receive_slot_t* slot = cache->idle;
  if (slot) {
    cache->idle = slot->next;
    slot->next = NULL;
  }
  iree_slim_mutex_unlock(&cache->mutex);
  iree_status_t status = iree_ok_status();
  if (!slot) {
    status = loom_serve_http_receive_cache_grow(cache, &slot);
  }
  if (iree_status_is_ok(status)) {
    loom_serve_http_receive_cache_retain(cache);
    *out_slot = slot;
  }
  IREE_TRACE_ZONE_END(z0);
  return status;
}

iree_async_buffer_pool_t* loom_serve_http_receive_slot_buffers(
    loom_serve_http_receive_slot_t* slot) {
  return slot->buffers;
}

void loom_serve_http_receive_slot_recycle(
    loom_serve_http_receive_slot_t* slot) {
  if (!slot) {
    return;
  }
  loom_serve_http_receive_cache_t* cache = slot->cache;
  iree_slim_mutex_lock(&cache->mutex);
  slot->next = cache->idle;
  cache->idle = slot;
  iree_slim_mutex_unlock(&cache->mutex);
  loom_serve_http_receive_cache_release(cache);
}
