// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef IREE_ASYNC_FILE_H_
#define IREE_ASYNC_FILE_H_

#include "iree/async/primitive.h"
#include "iree/base/api.h"
#include "iree/base/internal/atomics.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

typedef struct iree_async_proactor_t iree_async_proactor_t;

// Properties of an already-open native file. Import declares these properties;
// it does not change the native handle's mode.
typedef uint32_t iree_async_file_flags_t;
enum iree_async_file_flag_bits_e {
  IREE_ASYNC_FILE_FLAG_NONE = 0u,
  // Native uncached I/O: O_DIRECT, FILE_FLAG_NO_BUFFERING, or F_NOCACHE.
  // This is independent of write durability and is not a zero-copy guarantee.
  IREE_ASYNC_FILE_FLAG_DIRECT = 1u << 0,
};

// Required geometry for a single native read or write request. Each alignment
// is a positive power of two in bytes; one means byte-granular access. These
// are legality requirements, not preferred transfer sizes or atomicity units.
typedef struct iree_async_file_transfer_requirements_t {
  // Required alignment of the first byte of the host buffer.
  iree_host_size_t buffer_alignment;
  // Required alignment of the file byte offset.
  uint64_t offset_alignment;
  // Required alignment of the requested byte count, not the completed count.
  iree_host_size_t length_alignment;
} iree_async_file_transfer_requirements_t;

// Geometry for positioned file transfers under the file's declared mode.
// Access permissions remain those of the native handle; these requirements do
// not grant read or write access.
typedef struct iree_async_file_requirements_t {
  // Requirements for native reads.
  iree_async_file_transfer_requirements_t read;
  // Requirements for native writes.
  iree_async_file_transfer_requirements_t write;
} iree_async_file_requirements_t;

//===----------------------------------------------------------------------===//
// File
//===----------------------------------------------------------------------===//

// A proactor-managed async file handle.
// Created via iree_async_file_import() (importing an existing handle)
// or as the result of an async open operation.
//
// An iree_async_file_t wraps a platform file descriptor/handle and is bound to
// a specific proactor for async I/O operations. Files are created by importing
// an existing platform handle via iree_async_file_import(), or
// obtained as the result of an async open operation.
//
// All file I/O uses positioned semantics (pread/pwrite): each operation
// specifies its own offset, eliminating shared-position races between
// concurrent operations on the same file.
typedef struct iree_async_file_t {
  // Reference count controlling the file and native handle lifetime.
  iree_atomic_ref_count_t ref_count;

  // The proactor this file is bound to. All operations on this file must be
  // submitted to this proactor. Not retained (proactor outlives files).
  iree_async_proactor_t* proactor;

  // Underlying platform handle.
  iree_async_primitive_t primitive;

  // io_uring fixed file index for reduced syscall overhead (-1 if not
  // registered). Backend-specific optimization; ignored on other platforms.
  int32_t fixed_file_index;

  // Native mode declared at open/import; immutable for this file's lifetime.
  iree_async_file_flags_t flags;

  IREE_TRACE(char debug_path[256];)
} iree_async_file_t;

// Imports an existing platform handle as a proactor-managed file.
//
// Use this to bring externally-opened files under proactor management for
// async read/write operations. |flags| must describe the native handle's actual
// mode. The caller must not change that mode through this handle or an alias
// while the file is live. Windows handles must have FILE_FLAG_OVERLAPPED set.
//
// Availability:
//   generic | io_uring | IOCP | kqueue
//   yes     | yes      | yes  | yes
//
// Ownership:
//   The proactor takes logical ownership of the handle and will close it when
//   the file is released or a close operation completes. The caller must not
//   close the handle after a successful import. Failure leaves ownership with
//   the caller and sets |out_file| to NULL.
//
// Platform handles:
//   - POSIX: int fd (file descriptor from open())
//   - Windows: HANDLE (from CreateFile)
//
// Returns:
//   IREE_STATUS_OK: File imported successfully.
//   IREE_STATUS_INVALID_ARGUMENT: Invalid handle.
IREE_API_EXPORT iree_status_t iree_async_file_import(
    iree_async_proactor_t* proactor, iree_async_primitive_t primitive,
    iree_async_file_flags_t flags, iree_async_file_t** out_file);

// Queries complete read/write requirements for a borrowed native file handle.
// |flags| declares the handle's actual mode, as for file import. The handle
// need not be associated with a proactor; callers can establish requirements
// before importing it without changing its mode or transferring ownership.
// On failure |out_requirements| is unchanged. UNAVAILABLE means that the native
// filesystem does not report a usable alignment contract; unknown requirements
// are never reported as unrestricted. Buffered files are byte-granular.
//
// This is a synchronous, cold setup query. It may issue filesystem metadata
// syscalls and block. Successful queries allocate no storage, take no library
// locks, and initialize no hidden cache. Callers retain the result when
// constructing their transfer pools; submission performs no requirements
// queries.
//
// Requirements apply to each submitted request. A successful short read can
// return a byte count that is not aligned; advancing the buffer/offset by that
// count does not necessarily produce a legal continuation. Cache policy,
// durability, and absence of kernel copies are separate from these
// requirements.
IREE_API_EXPORT iree_status_t iree_async_file_query_requirements(
    iree_async_primitive_t primitive, iree_async_file_flags_t flags,
    iree_async_file_requirements_t* out_requirements);

// Increments the reference count.
IREE_API_EXPORT void iree_async_file_retain(iree_async_file_t* file);

// Decrements the reference count and destroys if it reaches zero.
IREE_API_EXPORT void iree_async_file_release(iree_async_file_t* file);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // IREE_ASYNC_FILE_H_
