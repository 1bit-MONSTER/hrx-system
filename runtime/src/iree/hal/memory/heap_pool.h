// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef IREE_HAL_MEMORY_HEAP_POOL_H_
#define IREE_HAL_MEMORY_HEAP_POOL_H_

#include "iree/base/api.h"
#include "iree/hal/pool.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

// Creates an unbounded host-only pool backed by |data_allocator|.
//
// The pool has no device group or queue-family scope. Its buffers support only
// the requested public host mappings and cannot be submitted to device queues.
// Allocation failures return as statuses; the pool never reports transient
// capacity exhaustion and therefore has no capacity notification or progress
// thread. Released allocations return directly to |data_allocator|.
//
// |host_access| must request at least one mapping mode and corresponding read
// or write access. Ordinary heap storage always achieves write-back caching.
// Both allocators must remain valid until the pool and every derived buffer are
// released.
IREE_API_EXPORT iree_status_t iree_hal_heap_pool_create(
    iree_hal_pool_host_access_t host_access, iree_allocator_t data_allocator,
    iree_allocator_t host_allocator, iree_hal_pool_t** out_pool);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // IREE_HAL_MEMORY_HEAP_POOL_H_
