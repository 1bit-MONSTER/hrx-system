// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// O_DIRECT requires the GNU fcntl declarations on glibc.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif  // _GNU_SOURCE

#include "iree/base/internal/file_io_posix.h"

#if IREE_FILE_IO_ENABLE && !defined(IREE_PLATFORM_WINDOWS)

#include <errno.h>
#include <fcntl.h>

iree_status_t iree_file_enable_direct_io(int fd) {
#if defined(IREE_PLATFORM_APPLE)
  int result = fcntl(fd, F_NOCACHE, 1);
#elif defined(O_DIRECT)
  int flags = fcntl(fd, F_GETFL);
  int result = flags < 0 ? -1 : fcntl(fd, F_SETFL, flags | O_DIRECT);
#else
  (void)fd;
  return iree_make_status(IREE_STATUS_UNAVAILABLE,
                          "native uncached file I/O is unavailable");
#endif
#if defined(IREE_PLATFORM_APPLE) || defined(O_DIRECT)
  if (result < 0) {
    int error = errno;
    if (error == EINVAL || error == ENOTSUP) {
      return iree_make_status(IREE_STATUS_UNAVAILABLE,
                              "file does not support native uncached I/O");
    }
    return iree_make_status(iree_status_code_from_errno(error),
                            "enabling native uncached file I/O failed");
  }
  return iree_ok_status();
#endif
}

#else

iree_status_t iree_file_enable_direct_io(int fd) {
  (void)fd;
  return iree_make_status(IREE_STATUS_UNAVAILABLE,
                          "POSIX file I/O is unavailable");
}

#endif  // IREE_FILE_IO_ENABLE && !IREE_PLATFORM_WINDOWS
