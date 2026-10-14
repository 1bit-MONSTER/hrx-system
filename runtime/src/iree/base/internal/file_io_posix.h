// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef IREE_BASE_INTERNAL_FILE_IO_POSIX_H_
#define IREE_BASE_INTERNAL_FILE_IO_POSIX_H_

#include "iree/base/api.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

// Enables native uncached I/O on a newly opened POSIX descriptor. The caller
// owns rollback and must not publish the descriptor until this succeeds. This
// changes the open file description (including any dup aliases), not an inode
// or filesystem policy. Write durability is unchanged.
iree_status_t iree_file_enable_direct_io(int fd);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // IREE_BASE_INTERNAL_FILE_IO_POSIX_H_
