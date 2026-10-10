// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef IREE_HAL_CTS_UTIL_POOL_TEST_UTIL_H_
#define IREE_HAL_CTS_UTIL_POOL_TEST_UTIL_H_

#include "iree/hal/device_group.h"
#include "iree/hal/memory/fixed_block_pool.h"

namespace iree::hal::cts {

// Creates a native slab pool scoped to one queue family.
iree_status_t CreateQueueSlabPool(iree_hal_device_group_t* device_group,
                                  const iree_hal_queue_family_t* queue_family,
                                  iree_hal_buffer_usage_t usage,
                                  iree_hal_pool_host_access_t host_access,
                                  iree_allocator_t host_allocator,
                                  iree_hal_pool_t** out_pool);

// Allocates one range from |backing_pool| and uses it as a finite block arena.
// The requested blocks_per_slab determines its initial capacity. The caller
// owns |backing_pool| and must release the finite pool before it.
iree_status_t CreateFiniteBlockPool(iree_hal_pool_t* backing_pool,
                                    iree_hal_fixed_block_pool_options_t options,
                                    iree_allocator_t host_allocator,
                                    iree_hal_pool_t** out_pool);

}  // namespace iree::hal::cts

#endif  // IREE_HAL_CTS_UTIL_POOL_TEST_UTIL_H_
