// Copyright 2025 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef IREE_IO_STDIO_UTIL_H_
#define IREE_IO_STDIO_UTIL_H_

#include <stdio.h>

#include "iree/base/api.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

// Opens a stdio file at the UTF-8 |path| using the fopen-compatible |mode|.
// The returned FILE* must be closed by the caller with fclose.
//
// Windows paths are opened through the wide-character CRT boundary and support
// extended-length paths without requiring a process manifest or system policy.
IREE_API_EXPORT iree_status_t
iree_io_stdio_file_open(iree_string_view_t path, const char* mode,
                        iree_allocator_t host_allocator, FILE** out_file);

//===----------------------------------------------------------------------===//
// Platform Support
//===----------------------------------------------------------------------===//

#if defined(IREE_PLATFORM_WINDOWS)

#include <fcntl.h>
#include <io.h>

#define IREE_IO_SET_BINARY_MODE(handle) _setmode(_fileno(handle), O_BINARY)

#define iree_dup _dup
#define iree_close _close
#define iree_fdopen _fdopen

#define iree_fseek _fseeki64
#define iree_ftell _ftelli64

#else

#include <unistd.h>

#define IREE_IO_SET_BINARY_MODE(handle) ((void)0)

#define iree_dup dup
#define iree_close close
#define iree_fdopen fdopen

#if _FILE_OFFSET_BITS == 64 || _POSIX_C_SOURCE >= 200112L
#define iree_fseek fseeko
#define iree_ftell ftello
#else
#define iree_fseek fseek
#define iree_ftell ftell
#endif  // 64-bit file offset support

#endif  // IREE_PLATFORM_WINDOWS

//===----------------------------------------------------------------------===//
// Error Handling
//===----------------------------------------------------------------------===//

// Makes a new status message ala iree_make_status but includes the error number
// and optional string message on platforms that support it.
#if defined(IREE_PLATFORM_WINDOWS)

#define iree_make_stdio_status(message)                                     \
  iree_make_status(iree_status_code_from_errno(errno), message " (%d: %s)", \
                   errno, strerror(errno))
#define iree_make_stdio_statusf(format, ...)                               \
  iree_make_status(iree_status_code_from_errno(errno), format " (%d: %s)", \
                   __VA_ARGS__, errno, strerror(errno))

#else

#define iree_make_stdio_status(...) \
  iree_make_status(IREE_STATUS_UNKNOWN, __VA_ARGS__)
#define iree_make_stdio_statusf iree_make_stdio_status

#endif  // IREE_PLATFORM_*

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // IREE_IO_STDIO_UTIL_H_
