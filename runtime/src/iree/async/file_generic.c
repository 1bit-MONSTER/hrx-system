// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/async/file_platform.h"

iree_status_t iree_async_file_query_direct_requirements(
    iree_async_primitive_t primitive,
    iree_async_file_requirements_t* out_requirements) {
  (void)primitive;
  (void)out_requirements;
  return iree_make_status(
      IREE_STATUS_UNAVAILABLE,
      "native direct I/O requirements are unavailable on this platform");
}
