// Copyright 2020 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <stddef.h>

#include "iree/base/api.h"
#include "iree/hal/allocator.h"
#include "iree/hal/resource.h"

static const iree_hal_memory_type_t iree_hal_heap_allocator_memory_type =
    IREE_HAL_MEMORY_TYPE_DEVICE_LOCAL | IREE_HAL_MEMORY_TYPE_HOST_LOCAL |
    IREE_HAL_MEMORY_TYPE_HOST_COHERENT | IREE_HAL_MEMORY_TYPE_HOST_CACHED;

typedef struct iree_hal_heap_allocator_t {
  iree_hal_resource_t resource;
  iree_allocator_t host_allocator;
} iree_hal_heap_allocator_t;

static const iree_hal_allocator_vtable_t iree_hal_heap_allocator_vtable;

iree_hal_heap_allocator_t* iree_hal_heap_allocator_cast(
    iree_hal_allocator_t* IREE_RESTRICT base_value) {
  return (iree_hal_heap_allocator_t*)base_value;
}

IREE_API_EXPORT iree_status_t iree_hal_allocator_create_heap(
    iree_allocator_t host_allocator, iree_hal_allocator_t** out_allocator) {
  IREE_ASSERT_ARGUMENT(out_allocator);
  *out_allocator = NULL;
  IREE_TRACE_ZONE_BEGIN(z0);

  iree_hal_heap_allocator_t* allocator = NULL;
  iree_status_t status = iree_allocator_malloc(
      host_allocator, sizeof(*allocator), (void**)&allocator);
  if (iree_status_is_ok(status)) {
    iree_hal_resource_initialize(&iree_hal_heap_allocator_vtable,
                                 &allocator->resource);
    allocator->host_allocator = host_allocator;
    *out_allocator = (iree_hal_allocator_t*)allocator;
  }

  IREE_TRACE_ZONE_END(z0);
  return status;
}

static void iree_hal_heap_allocator_destroy(
    iree_hal_allocator_t* IREE_RESTRICT base_allocator) {
  iree_hal_heap_allocator_t* allocator =
      iree_hal_heap_allocator_cast(base_allocator);
  iree_allocator_t host_allocator = allocator->host_allocator;
  IREE_TRACE_ZONE_BEGIN(z0);

  iree_allocator_free(host_allocator, allocator);

  IREE_TRACE_ZONE_END(z0);
}

static iree_allocator_t iree_hal_heap_allocator_host_allocator(
    const iree_hal_allocator_t* IREE_RESTRICT base_allocator) {
  iree_hal_heap_allocator_t* allocator =
      (iree_hal_heap_allocator_t*)base_allocator;
  return allocator->host_allocator;
}

static void iree_hal_heap_allocator_query_statistics(
    iree_hal_allocator_t* IREE_RESTRICT base_allocator,
    iree_hal_allocator_statistics_t* IREE_RESTRICT out_statistics) {
  (void)base_allocator;
  (void)out_statistics;
}

static iree_status_t iree_hal_heap_allocator_query_memory_heaps(
    iree_hal_allocator_t* IREE_RESTRICT base_allocator,
    iree_host_size_t capacity,
    iree_hal_allocator_memory_heap_t* IREE_RESTRICT heaps,
    iree_host_size_t* IREE_RESTRICT out_count) {
  const iree_host_size_t count = 1;
  if (out_count) {
    *out_count = count;
  }
  if (capacity < count) {
    // NOTE: lightweight as this is hit in normal pre-sizing usage.
    return iree_status_from_code(IREE_STATUS_OUT_OF_RANGE);
  }
  heaps[0] = (iree_hal_allocator_memory_heap_t){
      .type = iree_hal_heap_allocator_memory_type,
      .allowed_usage = IREE_HAL_BUFFER_USAGE_TRANSFER |
                       IREE_HAL_BUFFER_USAGE_DISPATCH |
                       IREE_HAL_BUFFER_USAGE_SHARING_EXPORT |
                       IREE_HAL_BUFFER_USAGE_SHARING_REPLICATE |
                       IREE_HAL_BUFFER_USAGE_SHARING_CONCURRENT |
                       IREE_HAL_BUFFER_USAGE_SHARING_IMMUTABLE |
                       IREE_HAL_BUFFER_USAGE_MAPPING_SCOPED |
                       IREE_HAL_BUFFER_USAGE_MAPPING_PERSISTENT |
                       IREE_HAL_BUFFER_USAGE_MAPPING_OPTIONAL |
                       IREE_HAL_BUFFER_USAGE_MAPPING_ACCESS_RANDOM |
                       IREE_HAL_BUFFER_USAGE_MAPPING_ACCESS_SEQUENTIAL_WRITE,
      .atomic_operations = iree_hal_atomic_operation_capabilities_for_host(
          IREE_HAL_ATOMIC_OPERATION_FLAGS_ALL),
      .max_allocation_size = ~(iree_device_size_t)0,
      .min_alignment = IREE_HAL_HEAP_BUFFER_ALIGNMENT,
  };
  return iree_ok_status();
}

static bool iree_hal_heap_allocator_supports_virtual_memory(
    iree_hal_allocator_t* IREE_RESTRICT base_allocator) {
  return false;
}

static iree_status_t iree_hal_heap_allocator_virtual_memory_query_granularity(
    iree_hal_allocator_t* IREE_RESTRICT base_allocator,
    iree_hal_physical_memory_params_t params,
    iree_device_size_t* IREE_RESTRICT out_minimum_page_size,
    iree_device_size_t* IREE_RESTRICT out_recommended_page_size) {
  *out_minimum_page_size = 0;
  *out_recommended_page_size = 0;
  return iree_make_status(IREE_STATUS_UNAVAILABLE,
                          "heap allocator does not support virtual memory");
}

static iree_status_t iree_hal_heap_allocator_virtual_memory_reserve(
    iree_hal_allocator_t* IREE_RESTRICT base_allocator,
    iree_hal_queue_family_affinity_t queue_family_affinity,
    iree_device_size_t size,
    iree_hal_buffer_t** IREE_RESTRICT out_virtual_buffer) {
  *out_virtual_buffer = NULL;
  return iree_make_status(IREE_STATUS_UNAVAILABLE,
                          "heap allocator does not support virtual memory");
}

static iree_status_t iree_hal_heap_allocator_virtual_memory_release(
    iree_hal_allocator_t* IREE_RESTRICT base_allocator,
    iree_hal_buffer_t* IREE_RESTRICT virtual_buffer) {
  return iree_make_status(IREE_STATUS_UNAVAILABLE,
                          "heap allocator does not support virtual memory");
}

static iree_status_t iree_hal_heap_allocator_physical_memory_allocate(
    iree_hal_allocator_t* IREE_RESTRICT base_allocator,
    iree_hal_physical_memory_params_t params, iree_device_size_t size,
    iree_allocator_t host_allocator,
    iree_hal_physical_memory_t** IREE_RESTRICT out_physical_memory) {
  *out_physical_memory = NULL;
  return iree_make_status(IREE_STATUS_UNAVAILABLE,
                          "heap allocator does not support virtual memory");
}

static iree_status_t iree_hal_heap_allocator_physical_memory_free(
    iree_hal_allocator_t* IREE_RESTRICT base_allocator,
    iree_hal_physical_memory_t* IREE_RESTRICT physical_memory) {
  return iree_make_status(IREE_STATUS_UNAVAILABLE,
                          "heap allocator does not support virtual memory");
}

static iree_status_t iree_hal_heap_allocator_virtual_memory_map(
    iree_hal_allocator_t* IREE_RESTRICT base_allocator,
    iree_hal_buffer_t* IREE_RESTRICT virtual_buffer,
    iree_device_size_t virtual_offset,
    iree_hal_physical_memory_t* IREE_RESTRICT physical_memory,
    iree_device_size_t physical_offset, iree_device_size_t size) {
  return iree_make_status(IREE_STATUS_UNAVAILABLE,
                          "heap allocator does not support virtual memory");
}

static iree_status_t iree_hal_heap_allocator_virtual_memory_unmap(
    iree_hal_allocator_t* IREE_RESTRICT base_allocator,
    iree_hal_buffer_t* IREE_RESTRICT virtual_buffer,
    iree_device_size_t virtual_offset, iree_device_size_t size) {
  return iree_make_status(IREE_STATUS_UNAVAILABLE,
                          "heap allocator does not support virtual memory");
}

static iree_status_t iree_hal_heap_allocator_virtual_memory_protect(
    iree_hal_allocator_t* IREE_RESTRICT base_allocator,
    iree_hal_buffer_t* IREE_RESTRICT virtual_buffer,
    iree_device_size_t virtual_offset, iree_device_size_t size,
    iree_hal_queue_family_affinity_t queue_family_affinity,
    iree_hal_virtual_memory_access_scope_t access_scope,
    iree_hal_memory_protection_t protection) {
  return iree_make_status(IREE_STATUS_UNAVAILABLE,
                          "heap allocator does not support virtual memory");
}

static iree_status_t iree_hal_heap_allocator_virtual_memory_advise(
    iree_hal_allocator_t* IREE_RESTRICT base_allocator,
    iree_hal_buffer_t* IREE_RESTRICT virtual_buffer,
    iree_device_size_t virtual_offset, iree_device_size_t size,
    iree_hal_queue_family_affinity_t queue_family_affinity,
    iree_hal_memory_advice_t advice) {
  return iree_make_status(IREE_STATUS_UNAVAILABLE,
                          "heap allocator does not support virtual memory");
}

static const iree_hal_allocator_vtable_t iree_hal_heap_allocator_vtable = {
    .destroy = iree_hal_heap_allocator_destroy,
    .host_allocator = iree_hal_heap_allocator_host_allocator,
    .query_statistics = iree_hal_heap_allocator_query_statistics,
    .query_memory_heaps = iree_hal_heap_allocator_query_memory_heaps,
    .supports_virtual_memory = iree_hal_heap_allocator_supports_virtual_memory,
    .virtual_memory_query_granularity =
        iree_hal_heap_allocator_virtual_memory_query_granularity,
    .virtual_memory_reserve = iree_hal_heap_allocator_virtual_memory_reserve,
    .virtual_memory_release = iree_hal_heap_allocator_virtual_memory_release,
    .physical_memory_allocate =
        iree_hal_heap_allocator_physical_memory_allocate,
    .physical_memory_free = iree_hal_heap_allocator_physical_memory_free,
    .virtual_memory_map = iree_hal_heap_allocator_virtual_memory_map,
    .virtual_memory_unmap = iree_hal_heap_allocator_virtual_memory_unmap,
    .virtual_memory_protect = iree_hal_heap_allocator_virtual_memory_protect,
    .virtual_memory_advise = iree_hal_heap_allocator_virtual_memory_advise,
};
