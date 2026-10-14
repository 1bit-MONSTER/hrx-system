// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/async/cts/file/native_file.h"

#include "iree/async/primitive.h"
#include "iree/io/file_handle.h"

namespace iree::async::cts {

iree_status_t ImportNativeFile(iree_async_proactor_t* proactor,
                               iree_string_view_t path, NativeFileAccess access,
                               iree_async_file_flags_t flags,
                               iree_async_file_t** out_file) {
  *out_file = nullptr;
  iree_io_file_mode_t mode =
      IREE_IO_FILE_MODE_ASYNC | IREE_IO_FILE_MODE_SHARE_READ;
  if (iree_any_bit_set(access, kNativeFileAccessRead)) {
    mode |= IREE_IO_FILE_MODE_READ;
  }
  if (iree_any_bit_set(access, kNativeFileAccessWrite)) {
    mode |= IREE_IO_FILE_MODE_WRITE;
  }
  if (iree_any_bit_set(flags, IREE_ASYNC_FILE_FLAG_DIRECT)) {
    mode |= IREE_IO_FILE_MODE_DIRECT;
  }
  iree_io_file_handle_t* handle = nullptr;
  IREE_RETURN_IF_ERROR(
      iree_io_file_handle_open(mode, path, iree_allocator_system(), &handle));
  iree_async_primitive_t duplicate = iree_async_primitive_none();
  iree_status_t status = iree_async_primitive_dup(
      NativeFilePrimitive(iree_io_file_handle_value(handle).fd), &duplicate);
  if (iree_status_is_ok(status)) {
    iree_async_file_flags_t imported_flags = IREE_ASYNC_FILE_FLAG_NONE;
    if (iree_any_bit_set(iree_io_file_handle_mode(handle),
                         IREE_IO_FILE_MODE_DIRECT)) {
      imported_flags |= IREE_ASYNC_FILE_FLAG_DIRECT;
    }
    status =
        iree_async_file_import(proactor, duplicate, imported_flags, out_file);
    if (!iree_status_is_ok(status)) {
      iree_async_primitive_close(&duplicate);
    }
  }
  iree_io_file_handle_release(handle);
  return status;
}

}  // namespace iree::async::cts
