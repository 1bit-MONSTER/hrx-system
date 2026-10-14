// Copyright 2023 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/hal/utils/memory_file.h"

//===----------------------------------------------------------------------===//
// iree_hal_memory_file_storage_t
//===----------------------------------------------------------------------===//

// Reference-counted storage for memory file contents.
// This allows both the memory file and any intermediate/staging buffers that
// may reference it to keep the underlying storage live and not create cycles.
typedef struct iree_hal_memory_file_storage_t {
  // Reference count for this storage instance.
  iree_atomic_ref_count_t ref_count;
  // Used to allocate this structure.
  iree_allocator_t host_allocator;
  // Base file handle, retained.
  iree_io_file_handle_t* handle;
  // Host memory contents, unowned.
  iree_byte_span_t contents;
} iree_hal_memory_file_storage_t;

static iree_status_t iree_hal_memory_file_storage_create(
    iree_io_file_handle_t* handle, iree_byte_span_t contents,
    iree_allocator_t host_allocator,
    iree_hal_memory_file_storage_t** out_storage) {
  IREE_ASSERT_ARGUMENT(handle);
  IREE_ASSERT_ARGUMENT(out_storage);
  *out_storage = NULL;
  IREE_TRACE_ZONE_BEGIN(z0);

  iree_hal_memory_file_storage_t* storage = NULL;
  IREE_RETURN_AND_END_ZONE_IF_ERROR(
      z0, iree_allocator_malloc(host_allocator, sizeof(*storage),
                                (void**)&storage));
  iree_atomic_ref_count_init(&storage->ref_count);
  storage->host_allocator = host_allocator;
  storage->handle = handle;
  iree_io_file_handle_retain(storage->handle);
  storage->contents = contents;

  *out_storage = storage;
  IREE_TRACE_ZONE_END(z0);
  return iree_ok_status();
}

static void iree_hal_memory_file_storage_destroy(
    iree_hal_memory_file_storage_t* storage) {
  IREE_ASSERT_ARGUMENT(storage);
  IREE_TRACE_ZONE_BEGIN(z0);
  iree_allocator_t host_allocator = storage->host_allocator;

  iree_io_file_handle_release(storage->handle);

  iree_allocator_free(host_allocator, storage);

  IREE_TRACE_ZONE_END(z0);
}

static void iree_hal_memory_file_storage_retain(
    iree_hal_memory_file_storage_t* storage) {
  if (IREE_LIKELY(storage)) {
    iree_atomic_ref_count_inc(&storage->ref_count);
  }
}

static void iree_hal_memory_file_storage_release(
    iree_hal_memory_file_storage_t* storage) {
  if (IREE_LIKELY(storage) &&
      iree_atomic_ref_count_dec(&storage->ref_count) == 1) {
    iree_hal_memory_file_storage_destroy(storage);
  }
}

//===----------------------------------------------------------------------===//
// iree_hal_memory_file_t
//===----------------------------------------------------------------------===//

typedef struct iree_hal_memory_file_t {
  iree_hal_resource_t resource;
  // Used to allocate this structure.
  iree_allocator_t host_allocator;
  // Allowed access bits.
  iree_hal_memory_access_t access;
  // Underlying storage container, retained.
  iree_hal_memory_file_storage_t* storage;
  // Optional imported buffer if it was possible to do so.
  // Not all implementations and not all buffers can be imported.
  iree_hal_buffer_t* imported_buffer;
} iree_hal_memory_file_t;

static const iree_hal_file_vtable_t iree_hal_memory_file_vtable;

static iree_hal_memory_file_t* iree_hal_memory_file_cast(
    iree_hal_file_t* IREE_RESTRICT base_value) {
  return (iree_hal_memory_file_t*)base_value;
}

static iree_status_t iree_hal_memory_file_try_import_buffer(
    iree_hal_memory_file_t* file, iree_byte_span_t contents,
    iree_hal_pool_t* storage_pool);

IREE_API_EXPORT iree_status_t iree_hal_memory_file_wrap(
    iree_hal_pool_t* storage_pool, iree_hal_memory_access_t access,
    iree_io_file_handle_t* handle, iree_hal_memory_file_flags_t flags,
    iree_allocator_t host_allocator, iree_hal_file_t** out_file) {
  IREE_ASSERT_ARGUMENT(out_file);
  *out_file = NULL;

  const iree_hal_memory_file_flags_t supported_flags =
      IREE_HAL_MEMORY_FILE_FLAG_REQUIRE_DEVICE_VISIBLE_STORAGE;
  if (IREE_UNLIKELY(flags & ~supported_flags)) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "unsupported memory file flags: 0x%08" PRIx32,
                            flags);
  }

  // For now we only support host allocations but could open other types that
  // may be backed by memory if desired.
  if (iree_io_file_handle_type(handle) !=
      IREE_IO_FILE_HANDLE_TYPE_HOST_ALLOCATION) {
    return iree_make_status(IREE_STATUS_UNIMPLEMENTED,
                            "support for wrapping non-host-allocation file "
                            "handles with memory files is not yet implemented");
  }

  IREE_TRACE_ZONE_BEGIN(z0);

  iree_byte_span_t contents = iree_io_file_handle_value(handle).host_allocation;

  // Note that iree_device_size_t (for device offsets/sizes) may be smaller than
  // iree_host_size_t (for host offsets/sizes) - if so we need to ensure the
  // bytes passed in will still fit in iree_device_size_t.
  if (contents.data_length > IREE_DEVICE_SIZE_MAX) {
    IREE_TRACE_ZONE_END(z0);
    return iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                            "device size too small to represent host contents");
  }

  // Allocate file handle; this just holds a reference to the storage and
  // (optionally) the imported buffer.
  iree_hal_memory_file_t* file = NULL;
  IREE_RETURN_AND_END_ZONE_IF_ERROR(
      z0, iree_allocator_malloc(host_allocator, sizeof(*file), (void**)&file));
  iree_hal_resource_initialize(&iree_hal_memory_file_vtable, &file->resource);
  file->host_allocator = host_allocator;
  file->access = access;
  file->storage = NULL;
  file->imported_buffer = NULL;

  // Create the underlying storage container that we use to manage the storage
  // lifetime independently from the file lifetime.
  iree_status_t status = iree_hal_memory_file_storage_create(
      handle, contents, host_allocator, &file->storage);

  // Try importing the memory file as a device-accessible storage buffer.
  // Absence of an admitted route is a normal host-only result. Once the pool
  // advertises the exact class, attachment failures are real failures and are
  // propagated instead of silently changing the selected path.
  const iree_hal_external_storage_class_t storage_class =
      iree_hal_make_external_storage_class(
          IREE_HAL_EXTERNAL_STORAGE_TYPE_HOST_POINTER);
  iree_hal_pool_import_requirements_t import_requirements;
  const bool can_import =
      storage_pool && iree_hal_pool_query_import_requirements(
                          storage_pool, storage_class, &import_requirements);
  if (iree_status_is_ok(status) && can_import) {
    status =
        iree_hal_memory_file_try_import_buffer(file, contents, storage_pool);
  }

  if (iree_status_is_ok(status) &&
      iree_all_bits_set(
          flags, IREE_HAL_MEMORY_FILE_FLAG_REQUIRE_DEVICE_VISIBLE_STORAGE) &&
      (!file->imported_buffer ||
       !iree_all_bits_set(iree_hal_buffer_memory_type(file->imported_buffer),
                          IREE_HAL_MEMORY_TYPE_DEVICE_VISIBLE))) {
    status = iree_make_status(
        IREE_STATUS_UNAVAILABLE,
        "host allocation could not be imported as device-visible storage");
  }

  if (iree_status_is_ok(status)) {
    *out_file = (iree_hal_file_t*)file;
  } else {
    iree_hal_file_release((iree_hal_file_t*)file);
  }
  IREE_TRACE_ZONE_END(z0);
  return status;
}

static void iree_hal_memory_file_destroy(
    iree_hal_file_t* IREE_RESTRICT base_file) {
  iree_hal_memory_file_t* file = iree_hal_memory_file_cast(base_file);
  iree_allocator_t host_allocator = file->host_allocator;
  IREE_TRACE_ZONE_BEGIN(z0);

  iree_hal_buffer_release(file->imported_buffer);
  file->imported_buffer = NULL;

  iree_hal_memory_file_storage_release(file->storage);

  iree_allocator_free(host_allocator, file);

  IREE_TRACE_ZONE_END(z0);
}

IREE_API_EXPORT bool iree_hal_memory_file_isa(iree_hal_file_t* file) {
  return iree_hal_resource_is((const iree_hal_resource_t*)file,
                              &iree_hal_memory_file_vtable);
}

IREE_API_EXPORT iree_status_t iree_hal_memory_file_contents(
    iree_hal_file_t* file, iree_byte_span_t* out_contents) {
  IREE_ASSERT_ARGUMENT(file);
  IREE_ASSERT_ARGUMENT(out_contents);
  *out_contents = iree_byte_span_empty();
  if (!iree_hal_memory_file_isa(file)) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "file is not a HAL memory file");
  }
  iree_hal_memory_file_t* memory_file = iree_hal_memory_file_cast(file);
  *out_contents = memory_file->storage->contents;
  return iree_ok_status();
}

// Releases the underlying file storage after the buffer using it is released.
static void iree_hal_memory_file_buffer_release(
    void* user_data, iree_hal_external_storage_class_t storage_class,
    iree_hal_external_storage_payload_t payload) {
  (void)storage_class;
  (void)payload;
  iree_hal_memory_file_storage_release(
      (iree_hal_memory_file_storage_t*)user_data);
}

// Imports |contents| as a device-accessible HAL buffer through |storage_pool|.
static iree_status_t iree_hal_memory_file_try_import_buffer(
    iree_hal_memory_file_t* file, iree_byte_span_t contents,
    iree_hal_pool_t* storage_pool) {
  IREE_TRACE_ZONE_BEGIN(z0);

  iree_hal_buffer_usage_t required_usage = IREE_HAL_BUFFER_USAGE_NONE;
  if (iree_any_bit_set(file->access, IREE_HAL_MEMORY_ACCESS_READ)) {
    required_usage |= IREE_HAL_BUFFER_USAGE_TRANSFER_SOURCE;
  }
  if (iree_any_bit_set(file->access, IREE_HAL_MEMORY_ACCESS_WRITE)) {
    required_usage |= IREE_HAL_BUFFER_USAGE_TRANSFER_TARGET;
  }

  iree_hal_external_storage_t external_buffer = {
      .storage_class = iree_hal_make_external_storage_class(
          IREE_HAL_EXTERNAL_STORAGE_TYPE_HOST_POINTER),
      .payload.host_pointer = contents.data,
      .byte_length = (iree_device_size_t)contents.data_length,
      .host_cacheability = IREE_HAL_HOST_CACHEABILITY_WRITE_BACK,
      .maintenance_range = iree_hal_make_buffer_range(
          0, (iree_device_size_t)contents.data_length),
      .release = iree_hal_memory_file_buffer_release,
      .release_user_data = file->storage,
  };

  // NOTE: we make the buffer retain the underlying storage.
  // We have to handle the case where the import fails and we need to balance
  // the retain we did below.
  iree_hal_memory_file_storage_retain(file->storage);
  iree_status_t status = iree_hal_pool_import_buffer(
      storage_pool, &external_buffer,
      iree_hal_make_buffer_range(0, IREE_HAL_WHOLE_BUFFER),
      &file->imported_buffer);
  if (iree_status_is_ok(status)) {
    status = iree_hal_buffer_validate_access(
        iree_hal_buffer_allowed_access(file->imported_buffer), file->access);
  }
  if (iree_status_is_ok(status)) {
    status = iree_hal_buffer_validate_usage(
        iree_hal_buffer_allowed_usage(file->imported_buffer), required_usage);
  }
  if (iree_status_is_ok(status)) {
    file->imported_buffer->allowed_access = file->access;
    file->imported_buffer->allowed_usage = required_usage;
  }
  if (!iree_status_is_ok(status)) {
    iree_hal_buffer_release(file->imported_buffer);
    file->imported_buffer = NULL;
    if (external_buffer.storage_class.type !=
        IREE_HAL_EXTERNAL_STORAGE_TYPE_NONE) {
      iree_hal_external_storage_release(&external_buffer);
    }
  }

  IREE_TRACE({
    if (iree_status_is_ok(status)) {
      IREE_TRACE_ZONE_APPEND_TEXT(z0, "import success");
    } else {
      IREE_TRACE_ZONE_APPEND_TEXT(z0, "import failure");
      IREE_TRACE_ZONE_APPEND_TEXT(
          z0, iree_status_code_string(iree_status_code(status)));
    }
  });
  IREE_TRACE_ZONE_END(z0);
  return status;
}

static iree_hal_memory_access_t iree_hal_memory_file_allowed_access(
    iree_hal_file_t* base_file) {
  iree_hal_memory_file_t* file = iree_hal_memory_file_cast(base_file);
  return file->access;
}

static uint64_t iree_hal_memory_file_length(iree_hal_file_t* base_file) {
  iree_hal_memory_file_t* file = iree_hal_memory_file_cast(base_file);
  return file->storage->contents.data_length;
}

static const iree_async_file_requirements_t* iree_hal_memory_file_requirements(
    iree_hal_file_t* base_file) {
  (void)base_file;
  static const iree_async_file_requirements_t requirements = {
      .read = {1, 1, 1}, .write = {1, 1, 1}};
  return &requirements;
}

static iree_hal_buffer_t* iree_hal_memory_file_storage_buffer(
    iree_hal_file_t* base_file) {
  iree_hal_memory_file_t* file = iree_hal_memory_file_cast(base_file);
  return file->imported_buffer;
}

static iree_async_file_t* iree_hal_memory_file_async_handle(
    iree_hal_file_t* base_file) {
  // Memory files use storage_buffer for zero-copy transfer via queue_copy.
  return NULL;
}

static bool iree_hal_memory_file_supports_synchronous_io(
    iree_hal_file_t* base_file) {
  // Memory files always support synchronous IO.
  return true;
}

static iree_status_t iree_hal_memory_file_read(iree_hal_file_t* base_file,
                                               uint64_t file_offset,
                                               iree_byte_span_t target) {
  iree_hal_memory_file_t* file = iree_hal_memory_file_cast(base_file);
  memcpy(target.data, file->storage->contents.data + file_offset,
         target.data_length);
  return iree_ok_status();
}

static iree_status_t iree_hal_memory_file_write(iree_hal_file_t* base_file,
                                                uint64_t file_offset,
                                                iree_const_byte_span_t source) {
  iree_hal_memory_file_t* file = iree_hal_memory_file_cast(base_file);
  memcpy(file->storage->contents.data + file_offset, source.data,
         source.data_length);
  return iree_ok_status();
}

static const iree_hal_file_vtable_t iree_hal_memory_file_vtable = {
    .destroy = iree_hal_memory_file_destroy,
    .allowed_access = iree_hal_memory_file_allowed_access,
    .length = iree_hal_memory_file_length,
    .requirements = iree_hal_memory_file_requirements,
    .storage_buffer = iree_hal_memory_file_storage_buffer,
    .async_handle = iree_hal_memory_file_async_handle,
    .supports_synchronous_io = iree_hal_memory_file_supports_synchronous_io,
    .read = iree_hal_memory_file_read,
    .write = iree_hal_memory_file_write,
};
