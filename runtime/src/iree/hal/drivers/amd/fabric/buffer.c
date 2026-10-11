// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/hal/drivers/amd/fabric/buffer.h"

#include "iree/hal/drivers/amd/status.h"

typedef struct iree_hal_amd_fabric_buffer_t {
  // Logical HAL allocation and view metadata.
  iree_hal_buffer_t base;
  // Borrowed core API table, dominated by the owning device group.
  const amdf_api_t* api;
  // Allocator owning the HAL wrapper.
  iree_allocator_t host_allocator;
  // Native allocation and mapping borrowed from the storage owner.
  iree_hal_amd_fabric_memory_t* memory;
  // Byte position of this view in the native mapping.
  iree_device_size_t storage_offset;
  // Storage-owner callback invoked before freeing this wrapper.
  iree_hal_buffer_release_callback_t release_callback;
} iree_hal_amd_fabric_buffer_t;

static const iree_hal_buffer_vtable_t iree_hal_amd_fabric_buffer_vtable;

static void iree_hal_amd_fabric_buffer_destroy(iree_hal_buffer_t* base_buffer) {
  iree_hal_amd_fabric_buffer_t* buffer =
      (iree_hal_amd_fabric_buffer_t*)base_buffer;
  if (buffer->release_callback.fn) {
    buffer->release_callback.fn(buffer->release_callback.user_data,
                                base_buffer);
  }
  iree_allocator_free(buffer->host_allocator, buffer);
}

static iree_status_t iree_hal_amd_fabric_buffer_map_range(
    iree_hal_buffer_t* base_buffer, iree_hal_mapping_mode_t mode,
    iree_hal_memory_access_t access, iree_hal_buffer_map_flags_t flags,
    iree_device_size_t offset, iree_device_size_t length,
    iree_hal_buffer_mapping_t* mapping) {
  iree_hal_amd_fabric_buffer_t* buffer =
      (iree_hal_amd_fabric_buffer_t*)base_buffer;
  if (!buffer->memory->mapping) {
    return iree_make_status(IREE_STATUS_UNAVAILABLE,
                            "AMD backing has no host view");
  }
  mapping->contents = iree_make_byte_span(
      buffer->memory->contents.data + buffer->storage_offset + offset, length);
  return iree_ok_status();
}

static iree_status_t iree_hal_amd_fabric_buffer_unmap_range(
    iree_hal_buffer_t* buffer, iree_device_size_t offset,
    iree_device_size_t length, iree_hal_buffer_mapping_t* mapping) {
  return iree_ok_status();
}

static iree_status_t iree_hal_amd_fabric_buffer_invalidate_range(
    iree_hal_buffer_t* base_buffer, iree_device_size_t offset,
    iree_device_size_t length) {
  iree_hal_amd_fabric_buffer_t* buffer =
      (iree_hal_amd_fabric_buffer_t*)base_buffer;
  return IREE_HAL_AMD_STATUS_FROM_AMDF(
      buffer->api->host_mapping_cache_control(
          buffer->memory->mapping, AMDF_HOST_CACHE_OPERATION_INVALIDATE,
          buffer->storage_offset + offset, length),
      "host_mapping_cache_control(INVALIDATE)");
}

static iree_status_t iree_hal_amd_fabric_buffer_flush_range(
    iree_hal_buffer_t* base_buffer, iree_device_size_t offset,
    iree_device_size_t length) {
  iree_hal_amd_fabric_buffer_t* buffer =
      (iree_hal_amd_fabric_buffer_t*)base_buffer;
  return IREE_HAL_AMD_STATUS_FROM_AMDF(
      buffer->api->host_mapping_cache_control(
          buffer->memory->mapping, AMDF_HOST_CACHE_OPERATION_FLUSH,
          buffer->storage_offset + offset, length),
      "host_mapping_cache_control(FLUSH)");
}

static iree_status_t iree_hal_amd_fabric_buffer_export_range(
    iree_hal_buffer_t* base_buffer, iree_device_size_t local_byte_offset,
    iree_device_size_t local_byte_length,
    iree_hal_external_storage_class_t requested_class,
    iree_hal_external_storage_t* out_storage,
    iree_hal_buffer_range_t* out_logical_range) {
  iree_hal_amd_fabric_buffer_t* buffer =
      (iree_hal_amd_fabric_buffer_t*)base_buffer;
  const iree_hal_amd_fabric_memory_t* memory = buffer->memory;
  bool admitted = false;
  for (iree_host_size_t i = 0; i < memory->exports.class_count; ++i) {
    admitted |= iree_hal_external_storage_class_equal(
        memory->exports.classes[i], requested_class);
  }
  if (!admitted) {
    return iree_make_status(IREE_STATUS_UNAVAILABLE,
                            "export class was not admitted for this backing");
  }
  iree_hal_host_cacheability_t cacheability =
      IREE_HAL_HOST_CACHEABILITY_UNKNOWN;
  switch (memory->host_cacheability) {
    case AMDF_HOST_CACHEABILITY_WRITE_BACK:
      cacheability = IREE_HAL_HOST_CACHEABILITY_WRITE_BACK;
      break;
    case AMDF_HOST_CACHEABILITY_WRITE_COMBINED:
      cacheability = IREE_HAL_HOST_CACHEABILITY_WRITE_COMBINED;
      break;
    case AMDF_HOST_CACHEABILITY_UNCACHED:
      cacheability = IREE_HAL_HOST_CACHEABILITY_UNCACHED;
      break;
    default:
      break;
  }
  *out_storage = (iree_hal_external_storage_t){
      .storage_class = requested_class,
      .payload = {.host_pointer = memory->contents.data},
      .byte_length = memory->contents.data_length,
      .host_cacheability = cacheability,
      .maintenance_range = memory->maintenance_range,
  };
  if (base_buffer->memory.backing) {
    out_storage->physical_backing_id =
        base_buffer->memory.backing->physical_backing_id;
  }
  // The pointer borrows the mapping. The caller retains its source buffer or
  // allocation for every external use, including a second native registration.
  *out_logical_range = iree_hal_make_buffer_range(
      buffer->storage_offset + local_byte_offset, local_byte_length);
  return iree_ok_status();
}

static const iree_hal_buffer_vtable_t iree_hal_amd_fabric_buffer_vtable = {
    .destroy = iree_hal_amd_fabric_buffer_destroy,
    .export_range = iree_hal_amd_fabric_buffer_export_range,
    .map_range = iree_hal_amd_fabric_buffer_map_range,
    .unmap_range = iree_hal_amd_fabric_buffer_unmap_range,
    .invalidate_range = iree_hal_amd_fabric_buffer_invalidate_range,
    .flush_range = iree_hal_amd_fabric_buffer_flush_range,
};

iree_status_t iree_hal_amd_fabric_buffer_wrap(
    iree_hal_buffer_placement_t placement, const amdf_api_t* api,
    iree_hal_amd_fabric_buffer_storage_t storage,
    iree_device_size_t allocation_size, iree_hal_memory_type_t memory_type,
    iree_hal_memory_access_t allowed_access,
    iree_hal_buffer_usage_t allowed_usage,
    iree_hal_buffer_release_callback_t release_callback,
    iree_allocator_t host_allocator, iree_hal_buffer_t** out_buffer) {
  *out_buffer = NULL;
  iree_hal_amd_fabric_buffer_t* buffer = NULL;
  IREE_RETURN_IF_ERROR(
      iree_allocator_malloc(host_allocator, sizeof(*buffer), (void**)&buffer));
  iree_hal_buffer_initialize(placement, &buffer->base, allocation_size, 0,
                             allocation_size, memory_type, allowed_access,
                             allowed_usage, &iree_hal_amd_fabric_buffer_vtable,
                             &buffer->base);
  buffer->api = api;
  buffer->host_allocator = host_allocator;
  buffer->memory = storage.memory;
  buffer->storage_offset = storage.offset;
  buffer->release_callback = release_callback;
  buffer->base.memory.bindings = storage.bindings;
  buffer->base.memory.binding_offset = storage.offset;
  buffer->base.host_binding_index = storage.host_binding_index;
  *out_buffer = &buffer->base;
  return iree_ok_status();
}

iree_status_t iree_hal_amd_fabric_buffer_invalidate_host_range(
    iree_hal_buffer_t* buffer, iree_device_size_t offset,
    iree_device_size_t length) {
  if (iree_all_bits_set(iree_hal_buffer_memory_type(buffer),
                        IREE_HAL_MEMORY_TYPE_HOST_COHERENT)) {
    return iree_ok_status();
  }
  const iree_hal_buffer_vtable_t* vtable =
      (const iree_hal_buffer_vtable_t*)buffer->resource.vtable;
  return vtable->invalidate_range(
      buffer, iree_hal_buffer_byte_offset(buffer) + offset, length);
}

iree_status_t iree_hal_amd_fabric_buffer_flush_host_range(
    iree_hal_buffer_t* buffer, iree_device_size_t offset,
    iree_device_size_t length) {
  if (iree_all_bits_set(iree_hal_buffer_memory_type(buffer),
                        IREE_HAL_MEMORY_TYPE_HOST_COHERENT)) {
    return iree_ok_status();
  }
  const iree_hal_buffer_vtable_t* vtable =
      (const iree_hal_buffer_vtable_t*)buffer->resource.vtable;
  return vtable->flush_range(
      buffer, iree_hal_buffer_byte_offset(buffer) + offset, length);
}
