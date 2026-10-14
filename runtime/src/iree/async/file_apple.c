// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/async/file_platform.h"

iree_status_t iree_async_file_query_direct_requirements(
    iree_async_primitive_t primitive,
    iree_async_file_requirements_t* out_requirements) {
  if (primitive.type != IREE_ASYNC_PRIMITIVE_TYPE_FD) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "file requirements query needs a file descriptor");
  }
  // F_NOCACHE does not restrict byte-granular positioned I/O. Neither the VM
  // page size nor fpathconf's recommended transfer alignment is a requirement.
  *out_requirements =
      (iree_async_file_requirements_t){.read = {1, 1, 1}, .write = {1, 1, 1}};
  return iree_ok_status();
}
