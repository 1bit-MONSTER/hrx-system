// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/hal/cts/util/pool_test_util.h"

#include "iree/hal/slab_pool.h"

namespace iree::hal::cts {

iree_status_t CreateQueueSlabPool(iree_hal_device_group_t* device_group,
                                  const iree_hal_queue_family_t* queue_family,
                                  iree_hal_buffer_usage_t usage,
                                  iree_allocator_t host_allocator,
                                  iree_hal_pool_t** out_pool) {
  const iree_hal_pool_family_access_t family_access = {
      /*.family=*/queue_family,
      /*.usage=*/usage,
  };
  const iree_hal_pool_scope_t scope = {
      /*.family_count=*/1,
      /*.families=*/&family_access,
  };
  iree_hal_slab_pool_options_t options;
  iree_hal_slab_pool_options_initialize(&options);
  options.trace_name = IREE_SV("cts.queue");
  return iree_hal_slab_pool_create(device_group, scope, &options,
                                   host_allocator, out_pool);
}

iree_status_t CreateFiniteBlockPool(iree_hal_pool_t* backing_pool,
                                    iree_hal_fixed_block_pool_options_t options,
                                    iree_allocator_t host_allocator,
                                    iree_hal_pool_t** out_pool) {
  *out_pool = nullptr;
  iree_hal_pool_reservation_request_t request;
  iree_status_t status = iree_hal_fixed_block_pool_query_backing_request(
      backing_pool, &options, &request);
  iree_hal_buffer_t* buffer = nullptr;
  if (iree_status_is_ok(status)) {
    status = iree_hal_pool_allocate_buffer(backing_pool, request,
                                           iree_infinite_timeout(), &buffer);
  }
  if (iree_status_is_ok(status)) {
    options.blocks_per_slab = 0;
    status = iree_hal_fixed_block_pool_create_from_buffer(
        buffer, 0, IREE_HAL_WHOLE_BUFFER, &options, host_allocator, out_pool);
  }
  iree_hal_buffer_release(buffer);
  return status;
}

}  // namespace iree::hal::cts
