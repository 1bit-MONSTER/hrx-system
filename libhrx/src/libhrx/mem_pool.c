// Copyright 2026 The HRX Authors
// SPDX-License-Identifier: Apache-2.0
//
// Memory pool implementation. Wraps IREE HAL pools to provide stream-ordered
// memory-management policy. Pool lifecycle, allocation, and attribute
// management are implemented here; async alloc/free sequencing remains in the
// binding layer because it requires stream host callback support.

#include "mem_pool.h"

#include <stdlib.h>
#include <string.h>

#include "hrx_internal.h"

static void hrx_mem_pool_refresh_stats_locked(hrx_mem_pool_t pool) {
  iree_hal_pool_stats_t backing_stats = {0};
  if (pool->hal_pool) {
    iree_hal_pool_query_stats(pool->hal_pool, &backing_stats);
  }
  const uint64_t previous_reserved_mem_current = pool->reserved_mem_current;
  pool->reserved_mem_current = backing_stats.bytes_committed;
  if (pool->reserved_mem_current > previous_reserved_mem_current) {
    pool->reserved_mem_high =
        iree_max(pool->reserved_mem_high, pool->reserved_mem_current);
  }
}

// Detaches an idle child after all reservation epochs have retired. The caller
// holds |pool->mutex| and releases the returned reference after unlocking.
static iree_hal_pool_t* hrx_mem_pool_take_idle_hal_pool_locked(
    hrx_mem_pool_t pool) {
  if (!pool->hal_pool || pool->inflight_allocation_count != 0) {
    return NULL;
  }
  iree_hal_pool_stats_t stats = {0};
  iree_hal_pool_query_stats(pool->hal_pool, &stats);
  if (stats.bytes_reserved != 0 || pool->allocation_budget_current != 0) {
    return NULL;
  }
  iree_hal_pool_t* hal_pool = pool->hal_pool;
  pool->hal_pool = NULL;
  pool->reserved_mem_current = 0;
  return hal_pool;
}

static iree_status_t hrx_mem_pool_ensure_hal_pool_locked(hrx_mem_pool_t pool) {
  if (pool->hal_pool) {
    return iree_ok_status();
  }
  return hrx_mem_pool_backing_create_pool(pool->device, &pool->hal_pool);
}

//===----------------------------------------------------------------------===//
// Lifecycle
//===----------------------------------------------------------------------===//

hrx_status_t hrx_mem_pool_create(hrx_device_t device,
                                 const hrx_mem_pool_props_t* props,
                                 hrx_mem_pool_t* out_pool) {
  if (!device || !props || !out_pool) {
    return hrx_make_status(HRX_STATUS_INVALID_ARGUMENT,
                           "device, props, or out_pool is NULL");
  }

  hrx_mem_pool_s* pool = (hrx_mem_pool_s*)calloc(1, sizeof(hrx_mem_pool_s));
  if (!pool) {
    return hrx_make_status(HRX_STATUS_OUT_OF_MEMORY,
                           "failed to allocate mem pool");
  }

  iree_atomic_ref_count_init(&pool->ref_count);
  pool->device = device;
  hrx_device_retain(pool->device);
  pool->props = *props;
  pool->release_threshold = 0;
  pool->inflight_allocation_count = 0;
  pool->reuse_allow_internal_dependencies = true;
  pool->reuse_follow_event_dependencies = true;
  pool->reuse_allow_opportunistic = true;
  pool->reserved_mem_current = 0;
  pool->reserved_mem_high = 0;
  pool->used_mem_current = 0;
  pool->used_mem_high = 0;
  pool->platform_handle = NULL;
  iree_slim_mutex_initialize(&pool->mutex);
  hrx_mem_pool_backing_attach(&device->mem_pool_backing, &pool->retention);

  *out_pool = pool;
  return hrx_ok_status();
}

static void hrx_mem_pool_destroy(hrx_mem_pool_s* pool) {
  iree_hal_pool_release(pool->hal_pool);
  hrx_mem_pool_backing_detach(&pool->device->mem_pool_backing,
                              &pool->retention);
  hrx_device_release(pool->device);
  iree_slim_mutex_deinitialize(&pool->mutex);
  free(pool);
}

void hrx_mem_pool_retain(hrx_mem_pool_t pool) {
  if (pool) {
    iree_atomic_ref_count_inc(&pool->ref_count);
  }
}

void hrx_mem_pool_release(hrx_mem_pool_t pool) {
  if (pool && iree_atomic_ref_count_dec(&pool->ref_count) == 1) {
    hrx_mem_pool_destroy(pool);
  }
}

//===----------------------------------------------------------------------===//
// Attributes
//===----------------------------------------------------------------------===//

hrx_status_t hrx_mem_pool_get_attribute(hrx_mem_pool_t pool,
                                        hrx_mem_pool_attr_t attr,
                                        uint64_t* out_value) {
  if (!pool || !out_value) {
    return hrx_make_status(HRX_STATUS_INVALID_ARGUMENT,
                           "pool or out_value is NULL");
  }
  *out_value = 0;

  iree_slim_mutex_lock(&pool->mutex);
  hrx_mem_pool_refresh_stats_locked(pool);

  hrx_status_t status = hrx_ok_status();
  switch (attr) {
    case HRX_MEM_POOL_ATTR_REUSE_FOLLOW_EVENT_DEPENDENCIES:
      *out_value = pool->reuse_follow_event_dependencies ? 1 : 0;
      break;
    case HRX_MEM_POOL_ATTR_REUSE_ALLOW_INTERNAL_DEPENDENCIES:
      *out_value = pool->reuse_allow_internal_dependencies ? 1 : 0;
      break;
    case HRX_MEM_POOL_ATTR_REUSE_ALLOW_OPPORTUNISTIC:
      *out_value = pool->reuse_allow_opportunistic ? 1 : 0;
      break;
    case HRX_MEM_POOL_ATTR_RELEASE_THRESHOLD:
      *out_value = pool->release_threshold;
      break;
    case HRX_MEM_POOL_ATTR_RESERVED_MEM_CURRENT:
      *out_value = pool->reserved_mem_current;
      break;
    case HRX_MEM_POOL_ATTR_RESERVED_MEM_HIGH:
      *out_value = pool->reserved_mem_high;
      break;
    case HRX_MEM_POOL_ATTR_USED_MEM_CURRENT:
      *out_value = pool->used_mem_current;
      break;
    case HRX_MEM_POOL_ATTR_USED_MEM_HIGH:
      *out_value = pool->used_mem_high;
      break;
    default:
      status = hrx_make_status(HRX_STATUS_INVALID_ARGUMENT,
                               "invalid memory pool attribute");
      break;
  }

  iree_slim_mutex_unlock(&pool->mutex);
  return status;
}

hrx_status_t hrx_mem_pool_set_attribute(hrx_mem_pool_t pool,
                                        hrx_mem_pool_attr_t attr,
                                        uint64_t value) {
  if (!pool) {
    return hrx_make_status(HRX_STATUS_INVALID_ARGUMENT, "pool is NULL");
  }

  iree_slim_mutex_lock(&pool->mutex);

  hrx_status_t status = hrx_ok_status();
  switch (attr) {
    case HRX_MEM_POOL_ATTR_REUSE_FOLLOW_EVENT_DEPENDENCIES:
      pool->reuse_follow_event_dependencies = value != 0;
      break;
    case HRX_MEM_POOL_ATTR_REUSE_ALLOW_INTERNAL_DEPENDENCIES:
      pool->reuse_allow_internal_dependencies = value != 0;
      break;
    case HRX_MEM_POOL_ATTR_REUSE_ALLOW_OPPORTUNISTIC:
      pool->reuse_allow_opportunistic = value != 0;
      break;
    case HRX_MEM_POOL_ATTR_RELEASE_THRESHOLD:
      pool->release_threshold = value;
      hrx_mem_pool_backing_set_retention(&pool->device->mem_pool_backing,
                                         &pool->retention, value);
      break;
    case HRX_MEM_POOL_ATTR_RESERVED_MEM_HIGH:
      if (value != 0) {
        status = hrx_make_status(
            HRX_STATUS_INVALID_ARGUMENT,
            "reserved memory high watermark must reset to zero");
      } else {
        pool->reserved_mem_high = 0;
      }
      break;
    case HRX_MEM_POOL_ATTR_USED_MEM_HIGH:
      if (value != 0) {
        status =
            hrx_make_status(HRX_STATUS_INVALID_ARGUMENT,
                            "used memory high watermark must reset to zero");
      } else {
        pool->used_mem_high = 0;
      }
      break;
    default:
      status = hrx_make_status(HRX_STATUS_INVALID_ARGUMENT,
                               "invalid or read-only memory pool attribute");
      break;
  }

  iree_slim_mutex_unlock(&pool->mutex);
  return status;
}

//===----------------------------------------------------------------------===//
// Trim
//===----------------------------------------------------------------------===//

hrx_status_t hrx_mem_pool_trim(hrx_mem_pool_t pool, size_t min_bytes_to_keep) {
  if (!pool) {
    return hrx_make_status(HRX_STATUS_INVALID_ARGUMENT, "pool is NULL");
  }
  iree_hal_pool_t* idle_hal_pool = NULL;
  iree_slim_mutex_lock(&pool->mutex);
  if (pool->hal_pool) {
    iree_hal_pool_trim(pool->hal_pool, IREE_HAL_POOL_TRIM_FLAG_EXCESS, 0);
  }
  hrx_mem_pool_backing_trim(&pool->device->mem_pool_backing, &pool->retention,
                            min_bytes_to_keep);
  hrx_mem_pool_refresh_stats_locked(pool);
  if (min_bytes_to_keep == 0) {
    idle_hal_pool = hrx_mem_pool_take_idle_hal_pool_locked(pool);
  }
  iree_slim_mutex_unlock(&pool->mutex);
  iree_hal_pool_release(idle_hal_pool);
  return hrx_ok_status();
}

hrx_status_t hrx_mem_pool_release_unused(hrx_mem_pool_t pool) {
  if (!pool) {
    return hrx_make_status(HRX_STATUS_INVALID_ARGUMENT, "pool is NULL");
  }
  iree_slim_mutex_lock(&pool->mutex);
  if (pool->hal_pool) {
    iree_hal_pool_trim(pool->hal_pool, IREE_HAL_POOL_TRIM_FLAG_EXCESS, 0);
  }
  hrx_mem_pool_backing_trim(&pool->device->mem_pool_backing, &pool->retention,
                            pool->release_threshold);
  hrx_mem_pool_refresh_stats_locked(pool);
  iree_slim_mutex_unlock(&pool->mutex);
  // Completion callbacks publish reclamation but retain the pool objects.
  // Destroying an empty stack here would join cold maintenance on the caller.
  return hrx_ok_status();
}

void hrx_mem_pool_record_logical_allocation(hrx_mem_pool_t pool, size_t size) {
  if (!pool || size == 0) {
    return;
  }

  iree_slim_mutex_lock(&pool->mutex);
  if (size >= UINT64_MAX - pool->used_mem_current) {
    pool->used_mem_current = UINT64_MAX;
  } else {
    pool->used_mem_current += size;
  }
  pool->used_mem_high = iree_max(pool->used_mem_high, pool->used_mem_current);
  iree_slim_mutex_unlock(&pool->mutex);
}

void hrx_mem_pool_record_logical_free(hrx_mem_pool_t pool, size_t size) {
  if (!pool || size == 0) {
    return;
  }

  iree_slim_mutex_lock(&pool->mutex);
  IREE_ASSERT(pool->used_mem_current >= size);
  pool->used_mem_current -= size;
  iree_slim_mutex_unlock(&pool->mutex);
}

void hrx_mem_pool_release_allocation_budget(hrx_mem_pool_t pool, size_t size) {
  if (!pool || size == 0) {
    return;
  }

  iree_slim_mutex_lock(&pool->mutex);
  IREE_ASSERT(pool->allocation_budget_current >= size);
  pool->allocation_budget_current -= size;
  iree_slim_mutex_unlock(&pool->mutex);
}

//===----------------------------------------------------------------------===//
// Allocation
//===----------------------------------------------------------------------===//

static iree_status_t hrx_mem_pool_allocate_hal_buffer(
    hrx_mem_pool_t pool, iree_device_size_t size,
    iree_hal_pool_t** out_hal_pool, iree_hal_buffer_t** out_buffer) {
  IREE_ASSERT_ARGUMENT(out_hal_pool);
  IREE_ASSERT_ARGUMENT(out_buffer);
  *out_hal_pool = NULL;
  *out_buffer = NULL;
  if (!pool) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT, "pool is NULL");
  }
  if (size == 0) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "allocation size must be > 0");
  }

  iree_slim_mutex_lock(&pool->mutex);
  iree_status_t status = iree_ok_status();
  if (pool->props.max_size != 0) {
    if (pool->allocation_budget_current > pool->props.max_size ||
        size > pool->props.max_size - pool->allocation_budget_current) {
      status = iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                                "memory pool allocation exceeds max size");
    }
  }
  if (iree_status_is_ok(status)) {
    status = hrx_mem_pool_ensure_hal_pool_locked(pool);
  }
  iree_hal_pool_t* hal_pool = NULL;
  if (iree_status_is_ok(status)) {
    if (pool->props.max_size != 0) {
      pool->allocation_budget_current += size;
    }
    hal_pool = pool->hal_pool;
    iree_hal_pool_retain(hal_pool);
    ++pool->inflight_allocation_count;
  }
  iree_slim_mutex_unlock(&pool->mutex);
  if (!iree_status_is_ok(status)) {
    return status;
  }

  const iree_hal_pool_reservation_request_t request = {
      .allocation_size = size,
  };
  status = iree_hal_pool_allocate_buffer(hal_pool, request,
                                         iree_immediate_timeout(), out_buffer);
  if (!iree_status_is_ok(status) &&
      iree_status_code(status) == IREE_STATUS_DEADLINE_EXCEEDED) {
    iree_status_free(status);
    status = iree_make_status(
        IREE_STATUS_RESOURCE_EXHAUSTED,
        "memory pool has no immediately reusable capacity for allocation");
  }
  iree_slim_mutex_lock(&pool->mutex);
  --pool->inflight_allocation_count;
  if (!iree_status_is_ok(status) && pool->props.max_size != 0) {
    IREE_ASSERT(pool->allocation_budget_current >= size);
    pool->allocation_budget_current -= size;
  }
  if (iree_status_is_ok(status)) {
    hrx_mem_pool_refresh_stats_locked(pool);
  }
  iree_slim_mutex_unlock(&pool->mutex);

  if (iree_status_is_ok(status)) {
    *out_hal_pool = hal_pool;
  } else {
    iree_hal_pool_release(hal_pool);
  }
  return status;
}

hrx_status_t hrx_mem_pool_allocate_buffer(hrx_mem_pool_t pool, size_t size,
                                          hrx_buffer_t* buffer) {
  HRX_TRACE_ZONE_BEGIN(z0, "hrx_mem_pool_allocate_buffer");
  HRX_TRACE_ZONE_APPEND_BYTES(z0, size);
  if (!buffer) {
    HRX_RETURN_AND_END_ZONE(
        z0, hrx_make_status(HRX_STATUS_INVALID_ARGUMENT, "buffer is NULL"));
  }
  *buffer = NULL;
  if (!pool) {
    HRX_RETURN_AND_END_ZONE(
        z0, hrx_make_status(HRX_STATUS_INVALID_ARGUMENT, "pool is NULL"));
  }

  iree_hal_pool_t* hal_pool = NULL;
  iree_hal_buffer_t* hal_buffer = NULL;
  iree_status_t status = hrx_mem_pool_allocate_hal_buffer(
      pool, (iree_device_size_t)size, &hal_pool, &hal_buffer);
  if (!iree_status_is_ok(status)) {
    HRX_RETURN_AND_END_ZONE(z0, hrx_status_from_iree(status));
  }

  hrx_buffer_t new_buffer = NULL;
  status = iree_allocator_malloc(iree_allocator_system(), sizeof(hrx_buffer_s),
                                 (void**)&new_buffer);
  if (!iree_status_is_ok(status)) {
    iree_hal_buffer_release(hal_buffer);
    iree_hal_pool_release(hal_pool);
    if (pool->props.max_size != 0) {
      hrx_mem_pool_release_allocation_budget(pool, size);
    }
    HRX_RETURN_AND_END_ZONE(z0, hrx_status_from_iree(status));
  }

  memset(new_buffer, 0, sizeof(*new_buffer));
  iree_atomic_ref_count_init(&new_buffer->ref_count);
  new_buffer->hal_buffer = hal_buffer;
  new_buffer->hal_pool = hal_pool;
  new_buffer->device = pool->device;
  hrx_device_retain(new_buffer->device);
  new_buffer->mem_type =
      (hrx_memory_type_t)iree_hal_buffer_memory_type(hal_buffer);
  new_buffer->size = size;
  if (pool->props.max_size != 0) {
    hrx_mem_pool_retain(pool);
    new_buffer->allocation_budget_pool = pool;
    new_buffer->allocation_budget_size = size;
  }
  *buffer = new_buffer;
  HRX_RETURN_AND_END_ZONE(z0, hrx_ok_status());
}
