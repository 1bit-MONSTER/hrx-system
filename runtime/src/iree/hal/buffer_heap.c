// Copyright 2020 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "iree/base/api.h"
#include "iree/hal/allocator.h"
#include "iree/hal/buffer.h"
#include "iree/hal/buffer_heap_impl.h"
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

typedef enum iree_hal_heap_buffer_storage_mode_e {
  // Allocated as a [metadata, data] slab.
  // The base metadata pointer must be freed with iree_allocator_free_aligned.
  // The data storage is not freed.
  IREE_HAL_HEAP_BUFFER_STORAGE_MODE_SLAB = 0u,
  // Allocated as split [metadata] and [data].
  // The base metadata pointer must be freed with iree_allocator_free.
  // The data storage must be freed with iree_allocator_free_aligned.
  IREE_HAL_HEAP_BUFFER_STORAGE_MODE_SPLIT = 1u,
  // Allocated as split [metadata] and an externally-owned [data].
  // The base metadata pointer must be freed with iree_allocator_free.
  // A user-provided buffer release callback is notified that the buffer is no
  // longer referencing the data.
  IREE_HAL_HEAP_BUFFER_STORAGE_MODE_EXTERNAL = 2u,
} iree_hal_heap_buffer_storage_mode_t;

typedef struct iree_hal_heap_buffer_t {
  // base.flags has the iree_hal_heap_buffer_storage_mode_t.
  iree_hal_buffer_t base;

  // Host allocator this buffer metadata structure was allocated from. May be
  // different than the data allocator used for the buffer payload.
  iree_allocator_t host_allocator;

  // Prepared host execution binding, shared by all views of this allocation.
  iree_hal_buffer_native_binding_t native_binding;

  union {
    // Used for IREE_HAL_HEAP_BUFFER_STORAGE_MODE_SPLIT.
    iree_allocator_t data_allocator;
    // Used for IREE_HAL_HEAP_BUFFER_STORAGE_MODE_EXTERNAL.
    iree_hal_buffer_release_callback_t release_callback;
  };

  // Optional statistics shared with the allocator.
  IREE_STATISTICS(iree_hal_heap_allocator_statistics_t* statistics;)
} iree_hal_heap_buffer_t;

static const iree_hal_buffer_vtable_t iree_hal_heap_buffer_vtable;

// Allocates a buffer with the metadata and storage split.
// This results in an additional host allocation but allows for user-overridden
// data storage allocations.
static iree_status_t iree_hal_heap_buffer_allocate_split(
    iree_host_size_t allocation_size, iree_host_size_t alignment,
    iree_allocator_t data_allocator, iree_allocator_t host_allocator,
    iree_hal_heap_buffer_t** out_buffer, iree_byte_span_t* out_data) {
  // Try allocating the storage first as it's the most likely to fail if OOM.
  // The storage alignment is independent of the metadata allocator.
  out_data->data_length = allocation_size;
  uint8_t* data_ptr = 0;
  IREE_RETURN_IF_ERROR(
      iree_allocator_malloc_aligned(data_allocator, allocation_size, alignment,
                                    /*offset=*/0, (void**)&data_ptr));
  out_data->data = data_ptr;

  // Allocate the host metadata wrapper with natural alignment.
  iree_status_t status = iree_allocator_malloc(
      host_allocator, sizeof(**out_buffer), (void**)out_buffer);
  if (!iree_status_is_ok(status)) {
    // Need to free the storage we just allocated.
    iree_allocator_free_aligned(data_allocator, out_data->data);
  }
  return status;
}

// Allocates a buffer with the metadata as a prefix to the storage.
// This results in a single allocation per buffer but requires that both the
// metadata and storage live together.
static iree_status_t iree_hal_heap_buffer_allocate_slab(
    iree_host_size_t allocation_size, iree_host_size_t alignment,
    iree_allocator_t host_allocator, iree_hal_heap_buffer_t** out_buffer,
    iree_byte_span_t* out_data) {
  // The metadata header is always aligned and we want to ensure it's padded
  // out to the max alignment.
  iree_hal_heap_buffer_t* buffer = NULL;
  iree_host_size_t header_size =
      iree_host_align(iree_sizeof_struct(*buffer), iree_max_align_t);
  iree_host_size_t total_size = 0;
  if (!iree_host_size_checked_add(header_size, allocation_size, &total_size)) {
    return iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                            "heap buffer allocation size overflow");
  }

  // Allocate with the data starting at offset header_size aligned to the
  // requested buffer alignment. The header itself will still be aligned
  // to the natural alignment but our buffer alignment is often much larger.
  IREE_RETURN_IF_ERROR(iree_allocator_malloc_aligned(
      host_allocator, total_size, alignment, header_size, (void**)&buffer));
  *out_buffer = buffer;

  uint8_t* data_ptr = (uint8_t*)buffer + header_size;
  *out_data = iree_make_byte_span(data_ptr, allocation_size);

  return iree_ok_status();
}

iree_status_t iree_hal_heap_buffer_create(
    iree_hal_heap_allocator_statistics_t* statistics,
    const iree_hal_buffer_params_t* params, iree_device_size_t allocation_size,
    iree_allocator_t data_allocator, iree_allocator_t host_allocator,
    iree_hal_buffer_t** out_buffer) {
  IREE_ASSERT_ARGUMENT(params);
  IREE_ASSERT_ARGUMENT(out_buffer);
  IREE_TRACE_ZONE_BEGIN(z0);

  // If the data and host allocators are the same we can allocate more
  // efficiently as a large slab. Otherwise we need to allocate both the
  // metadata and the storage independently.
  const bool same_allocator =
      memcmp(&data_allocator, &host_allocator, sizeof(data_allocator)) == 0;
  const iree_host_size_t alignment = (iree_host_size_t)iree_max(
      params->min_alignment, IREE_HAL_HEAP_BUFFER_ALIGNMENT);

  iree_hal_heap_buffer_t* buffer = NULL;
  iree_byte_span_t data = iree_byte_span_empty();
  iree_status_t status =
      same_allocator
          ? iree_hal_heap_buffer_allocate_slab(allocation_size, alignment,
                                               host_allocator, &buffer, &data)
          : iree_hal_heap_buffer_allocate_split(allocation_size, alignment,
                                                data_allocator, host_allocator,
                                                &buffer, &data);

  if (iree_status_is_ok(status)) {
    iree_hal_buffer_initialize(
        iree_hal_buffer_placement_undefined(), &buffer->base, allocation_size,
        0, allocation_size, params->type, params->access, params->usage,
        &iree_hal_heap_buffer_vtable, &buffer->base);
    buffer->host_allocator = host_allocator;
    buffer->native_binding.host_pointer = data.data;
    buffer->base.memory.bindings = &buffer->native_binding;
    buffer->base.host_binding_index = 0;

    if (same_allocator) {
      buffer->base.flags = IREE_HAL_HEAP_BUFFER_STORAGE_MODE_SLAB;
      buffer->data_allocator = iree_allocator_null();
    } else {
      buffer->base.flags = IREE_HAL_HEAP_BUFFER_STORAGE_MODE_SPLIT;
      buffer->data_allocator = data_allocator;
    }

    IREE_STATISTICS({
      if (statistics != NULL) {
        buffer->statistics = statistics;
        iree_slim_mutex_lock(&statistics->mutex);
        iree_hal_allocator_statistics_record_alloc(
            &statistics->base, params->type, allocation_size);
        iree_slim_mutex_unlock(&statistics->mutex);
      }
    });

    *out_buffer = &buffer->base;
  }

  IREE_TRACE_ZONE_END(z0);
  return status;
}

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

    // Notify the provided callback when the external data is no longer needed.
    buffer->base.flags = IREE_HAL_HEAP_BUFFER_STORAGE_MODE_EXTERNAL;
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

  IREE_STATISTICS({
    if (buffer->statistics != NULL) {
      iree_slim_mutex_lock(&buffer->statistics->mutex);
      iree_hal_allocator_statistics_record_free(&buffer->statistics->base,
                                                base_buffer->memory_type,
                                                base_buffer->allocation_size);
      iree_slim_mutex_unlock(&buffer->statistics->mutex);
    }
  });

  switch (buffer->base.flags) {
    case IREE_HAL_HEAP_BUFFER_STORAGE_MODE_SLAB: {
      iree_allocator_free_aligned(host_allocator, buffer);
      break;
    }
    case IREE_HAL_HEAP_BUFFER_STORAGE_MODE_SPLIT: {
      iree_allocator_free_aligned(buffer->data_allocator,
                                  buffer->native_binding.host_pointer);
      iree_allocator_free(host_allocator, buffer);
      break;
    }
    case IREE_HAL_HEAP_BUFFER_STORAGE_MODE_EXTERNAL: {
      if (buffer->release_callback.fn) {
        buffer->release_callback.fn(buffer->release_callback.user_data,
                                    base_buffer);
      }
      iree_allocator_free(host_allocator, buffer);
      break;
    }
    default:
      IREE_ASSERT_UNREACHABLE("unhandled buffer storage mode");
      break;
  }

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
  out_storage->host_cacheability = IREE_HAL_HOST_CACHEABILITY_WRITE_BACK;
  if (base_buffer->memory.backing) {
    out_storage->maintenance_range =
        base_buffer->memory.backing->maintenance_range;
    out_storage->physical_backing_id =
        base_buffer->memory.backing->physical_backing_id;
  } else {
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
    .recycle = iree_hal_buffer_recycle,
    .destroy = iree_hal_heap_buffer_destroy,
    .export_range = iree_hal_heap_buffer_export_range,
    .map_range = iree_hal_heap_buffer_map_range,
    .unmap_range = iree_hal_heap_buffer_unmap_range,
    .invalidate_range = iree_hal_heap_buffer_invalidate_range,
    .flush_range = iree_hal_heap_buffer_flush_range,
};
