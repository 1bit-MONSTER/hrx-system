// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/io/stdio_util.h"

#include "iree/base/internal/path.h"

#if IREE_FILE_IO_ENABLE

IREE_API_EXPORT iree_status_t
iree_io_stdio_file_open(iree_string_view_t path, const char* mode,
                        iree_allocator_t host_allocator, FILE** out_file) {
  IREE_ASSERT_ARGUMENT(mode);
  IREE_ASSERT_ARGUMENT(out_file);
  *out_file = NULL;
  IREE_TRACE_ZONE_BEGIN(z0);
  IREE_TRACE_ZONE_APPEND_TEXT(z0, path.data, path.size);

  if (iree_string_view_is_empty(path)) {
    IREE_TRACE_ZONE_END(z0);
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "file path must not be empty");
  }
  if (!path.data) {
    IREE_TRACE_ZONE_END(z0);
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "file path storage must not be NULL");
  }
  if (memchr(path.data, 0, path.size) != NULL) {
    IREE_TRACE_ZONE_END(z0);
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "file path contains an embedded NUL character");
  }

  iree_status_t status = iree_ok_status();
  FILE* file = NULL;
  int open_error = 0;
#if defined(IREE_PLATFORM_WINDOWS)
  wchar_t* win32_path = NULL;
  status = iree_file_path_to_win32(path, host_allocator, &win32_path);
  if (iree_status_is_ok(status)) {
    const iree_host_size_t mode_length = strlen(mode);
    wchar_t mode_wide[16] = {0};
    if (mode_length >= IREE_ARRAYSIZE(mode_wide)) {
      status = iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                                "stdio mode is too long");
    } else {
      for (iree_host_size_t i = 0; i < mode_length; ++i) {
        mode_wide[i] = (wchar_t)mode[i];
      }
      file = _wfopen(win32_path, mode_wide);
      if (file == NULL) {
        open_error = errno;
      }
    }
  }
  iree_allocator_free(host_allocator, win32_path);
#else
  char* path_cstring = NULL;
  status = iree_allocator_malloc(host_allocator, path.size + 1,
                                 (void**)&path_cstring);
  if (iree_status_is_ok(status)) {
    iree_string_view_to_cstring(path, path_cstring, path.size + 1);
    file = fopen(path_cstring, mode);
    if (file == NULL) {
      open_error = errno;
    }
  }
  iree_allocator_free(host_allocator, path_cstring);
#endif  // IREE_PLATFORM_WINDOWS

  if (iree_status_is_ok(status) && file == NULL) {
    if (open_error == 0) {
      status = iree_make_status(IREE_STATUS_UNKNOWN,
                                "unable to open file `%.*s` with mode `%s`",
                                (int)path.size, path.data, mode);
    } else {
      status = iree_make_status(
          iree_status_code_from_errno(open_error),
          "unable to open file `%.*s` with mode `%s` (%d: %s)", (int)path.size,
          path.data, mode, open_error, strerror(open_error));
    }
  }

  if (iree_status_is_ok(status)) {
    *out_file = file;
  }
  IREE_TRACE_ZONE_END(z0);
  return status;
}

#else

IREE_API_EXPORT iree_status_t
iree_io_stdio_file_open(iree_string_view_t path, const char* mode,
                        iree_allocator_t host_allocator, FILE** out_file) {
  IREE_ASSERT_ARGUMENT(mode);
  IREE_ASSERT_ARGUMENT(out_file);
  *out_file = NULL;
  return iree_make_status(IREE_STATUS_UNAVAILABLE,
                          "file support has been compiled out of this binary; "
                          "set IREE_FILE_IO_ENABLE=1 to include it");
}

#endif  // IREE_FILE_IO_ENABLE
