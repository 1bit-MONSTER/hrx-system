// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef IREE_ASYNC_FILE_PLATFORM_H_
#define IREE_ASYNC_FILE_PLATFORM_H_

#include "iree/async/file.h"

// Queries the native alignment contract for an already-open uncached file.
// The caller supplies temporary output storage and publishes it only on
// success.
iree_status_t iree_async_file_query_direct_requirements(
    iree_async_primitive_t primitive,
    iree_async_file_requirements_t* out_requirements);

#endif  // IREE_ASYNC_FILE_PLATFORM_H_
