// Copyright 2024 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef IREE_HAL_UTILS_FILE_REGISTRY_H_
#define IREE_HAL_UTILS_FILE_REGISTRY_H_

#include "iree/base/api.h"
#include "iree/hal/api.h"
#include "iree/io/file_handle.h"

typedef struct iree_async_proactor_t iree_async_proactor_t;

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

// Creates a file backed by |handle| using a common host implementation.
// Supported file handle types are determined based on compile configuration.
//
// Some implementations - such as for IREE_IO_FILE_HANDLE_TYPE_HOST_ALLOCATION -
// may import the backing storage directly into a device-accessible storage
// buffer using |storage_pool|. A NULL pool or a pool that does not admit the
// handle's storage class leaves the file on its native I/O path.
//
// If |proactor| is non-NULL, file descriptor-based files will be bound to the
// proactor for async I/O at construction time. The async handle is then
// available via iree_hal_file_async_handle().
IREE_API_EXPORT iree_status_t iree_hal_file_from_handle(
    iree_hal_pool_t* storage_pool, iree_hal_memory_access_t access,
    iree_io_file_handle_t* handle, iree_async_proactor_t* proactor,
    iree_allocator_t host_allocator, iree_hal_file_t** out_file);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // IREE_HAL_UTILS_FILE_REGISTRY_H_
