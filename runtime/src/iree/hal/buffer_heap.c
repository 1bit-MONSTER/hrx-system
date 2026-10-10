// Copyright 2020 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "iree/base/api.h"
#include "iree/hal/buffer.h"
#include "iree/hal/resource.h"

IREE_API_EXPORT const iree_hal_buffer_binding_layout_t*
iree_hal_heap_buffer_binding_layout(void) {
  static const uint16_t types[] = {IREE_HAL_BUFFER_INTERFACE_HOST};
  static const iree_hal_buffer_binding_layout_t layout = {
      .byte_length = sizeof(iree_hal_buffer_native_binding_t),
      .binding_count = IREE_ARRAYSIZE(types),
      .host_binding_index = 0,
      .types = types,
  };
  return &layout;
}

typedef struct iree_hal_heap_buffer_t {
  iree_hal_buffer_t base;

  // Host allocator used only for this buffer metadata structure.
  iree_allocator_t host_allocator;

  // Prepared host execution binding, shared by all views of this allocation.
  iree_hal_buffer_native_binding_t native_binding;

  // Callback notified when the buffer no longer references the storage.
  iree_hal_buffer_release_callback_t release_callback;
} iree_hal_heap_buffer_t;

static const iree_hal_buffer_vtable_t iree_hal_heap_buffer_vtable;

iree_status_t iree_hal_heap_buffer_wrap(
    iree_hal_buffer_placement_t placement, iree_hal_memory_type_t memory_type,
    iree_hal_memory_access_t allowed_access,
    iree_hal_buffer_usage_t allowed_usage, iree_device_size_t allocation_size,
    iree_byte_span_t data, iree_hal_buffer_release_callback_t release_callback,
    iree_allocator_t host_allocator, iree_hal_buffer_t** out_buffer) {
  IREE_ASSERT_ARGUMENT(out_buffer);
  IREE_TRACE_ZONE_BEGIN(z0);

  iree_hal_heap_buffer_t* buffer = NULL;
  iree_status_t status =
      iree_allocator_malloc(host_allocator, sizeof(*buffer), (void**)&buffer);
  if (iree_status_is_ok(status)) {
    iree_hal_buffer_initialize(placement, &buffer->base, allocation_size, 0,
                               data.data_length, memory_type, allowed_access,
                               allowed_usage, &iree_hal_heap_buffer_vtable,
                               &buffer->base);
    buffer->host_allocator = host_allocator;
    buffer->native_binding.host_pointer = data.data;
    buffer->base.memory.bindings = &buffer->native_binding;
    buffer->base.host_binding_index = 0;

    buffer->release_callback = release_callback;

    *out_buffer = &buffer->base;
  }

  IREE_TRACE_ZONE_END(z0);
  return status;
}

static void iree_hal_heap_buffer_destroy(iree_hal_buffer_t* base_buffer) {
  iree_hal_heap_buffer_t* buffer = (iree_hal_heap_buffer_t*)base_buffer;
  iree_allocator_t host_allocator = buffer->host_allocator;
  IREE_TRACE_ZONE_BEGIN(z0);

  if (buffer->release_callback.fn) {
    buffer->release_callback.fn(buffer->release_callback.user_data,
                                base_buffer);
  }
  iree_allocator_free(host_allocator, buffer);

  IREE_TRACE_ZONE_END(z0);
}

static iree_status_t iree_hal_heap_buffer_export_range(
    iree_hal_buffer_t* base_buffer, iree_device_size_t local_byte_offset,
    iree_device_size_t local_byte_length,
    iree_hal_external_storage_class_t requested_class,
    iree_hal_external_storage_t* out_storage,
    iree_hal_buffer_range_t* out_logical_range) {
  iree_hal_heap_buffer_t* buffer = (iree_hal_heap_buffer_t*)base_buffer;
  if (requested_class.type != IREE_HAL_EXTERNAL_STORAGE_TYPE_HOST_POINTER &&
      requested_class.type != IREE_HAL_EXTERNAL_STORAGE_TYPE_DEVICE_ADDRESS) {
    return iree_make_status(IREE_STATUS_UNAVAILABLE,
                            "external storage type not supported");
  }
  if (requested_class.provenance[0] || requested_class.provenance[1]) {
    return iree_make_status(
        IREE_STATUS_UNAVAILABLE,
        "heap pointer export has no matching opaque provenance");
  }

  // Heap storage is already mapped. Preserve the persistent-mapping contract
  // for borrowed pointers without creating another mapping or reference.
  IREE_RETURN_IF_ERROR(iree_hal_buffer_validate_memory_type(
      iree_hal_buffer_memory_type(base_buffer),
      IREE_HAL_MEMORY_TYPE_HOST_VISIBLE));
  IREE_RETURN_IF_ERROR(
      iree_hal_buffer_validate_usage(iree_hal_buffer_allowed_usage(base_buffer),
                                     IREE_HAL_BUFFER_USAGE_MAPPING_PERSISTENT));

  out_storage->storage_class = requested_class;
  out_storage->byte_length = base_buffer->allocation_size;
  if (base_buffer->memory.backing) {
    out_storage->host_cacheability =
        base_buffer->memory.backing->host_cacheability;
    out_storage->maintenance_range =
        base_buffer->memory.backing->maintenance_range;
    out_storage->physical_backing_id =
        base_buffer->memory.backing->physical_backing_id;
  } else {
    out_storage->host_cacheability = IREE_HAL_HOST_CACHEABILITY_WRITE_BACK;
    out_storage->maintenance_range = (iree_hal_buffer_range_t){
        .byte_length = base_buffer->allocation_size,
    };
  }
  if (requested_class.type == IREE_HAL_EXTERNAL_STORAGE_TYPE_HOST_POINTER) {
    out_storage->payload.host_pointer = buffer->native_binding.host_pointer;
  } else {
    out_storage->payload.device_address =
        (uint64_t)(uintptr_t)buffer->native_binding.host_pointer;
  }
  *out_logical_range = (iree_hal_buffer_range_t){
      .byte_offset = local_byte_offset,
      .byte_length = local_byte_length,
  };
  return iree_ok_status();
}

static iree_status_t iree_hal_heap_buffer_map_range(
    iree_hal_buffer_t* base_buffer, iree_hal_mapping_mode_t mapping_mode,
    iree_hal_memory_access_t memory_access, iree_hal_buffer_map_flags_t flags,
    iree_device_size_t local_byte_offset, iree_device_size_t local_byte_length,
    iree_hal_buffer_mapping_t* mapping) {
  iree_hal_heap_buffer_t* buffer = (iree_hal_heap_buffer_t*)base_buffer;
  mapping->contents = iree_make_byte_span(
      buffer->native_binding.host_pointer + local_byte_offset,
      local_byte_length);

  // If we mapped for discard scribble over the bytes. This is not a mandated
  // behavior but it will make debugging issues easier. Alternatively for
  // heap buffers we could reallocate them such that ASAN yells, but that
  // would only work if the entire buffer was discarded.
#ifndef NDEBUG
  if (iree_any_bit_set(flags, IREE_HAL_BUFFER_MAP_FLAG_DISCARD)) {
    memset(mapping->contents.data, 0xCD, local_byte_length);
  }
#endif  // !NDEBUG

  return iree_ok_status();
}

static iree_status_t iree_hal_heap_buffer_unmap_range(
    iree_hal_buffer_t* base_buffer, iree_device_size_t local_byte_offset,
    iree_device_size_t local_byte_length, iree_hal_buffer_mapping_t* mapping) {
  // No-op here as we always have the pointer.
  return iree_ok_status();
}

static iree_status_t iree_hal_heap_buffer_invalidate_range(
    iree_hal_buffer_t* base_buffer, iree_device_size_t local_byte_offset,
    iree_device_size_t local_byte_length) {
  iree_atomic_thread_fence(iree_memory_order_acquire);
  return iree_ok_status();
}

static iree_status_t iree_hal_heap_buffer_flush_range(
    iree_hal_buffer_t* base_buffer, iree_device_size_t local_byte_offset,
    iree_device_size_t local_byte_length) {
  iree_atomic_thread_fence(iree_memory_order_release);
  return iree_ok_status();
}

static const iree_hal_buffer_vtable_t iree_hal_heap_buffer_vtable = {
    .destroy = iree_hal_heap_buffer_destroy,
    .export_range = iree_hal_heap_buffer_export_range,
    .map_range = iree_hal_heap_buffer_map_range,
    .unmap_range = iree_hal_heap_buffer_unmap_range,
    .invalidate_range = iree_hal_heap_buffer_invalidate_range,
    .flush_range = iree_hal_heap_buffer_flush_range,
};
