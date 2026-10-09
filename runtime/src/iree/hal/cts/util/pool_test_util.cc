// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/hal/cts/util/pool_test_util.h"

#include "iree/hal/memory/passthrough_pool.h"

namespace iree::hal::cts {

iree_status_t CreateFiniteBlockPool(
    const iree_hal_queue_pool_backend_t& backend,
    iree_hal_fixed_block_pool_options_t options,
    iree_allocator_t host_allocator, iree_hal_pool_t** out_backing_pool,
    iree_hal_pool_t** out_pool) {
  *out_backing_pool = nullptr;
  *out_pool = nullptr;
  iree_hal_pool_t* backing_pool = nullptr;
  IREE_RETURN_IF_ERROR(iree_hal_passthrough_pool_create(
      {}, backend.slab_provider, backend.notification, backend.frontier_tracker,
      backend.maintenance, host_allocator, &backing_pool));
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
  if (iree_status_is_ok(status)) {
    *out_backing_pool = backing_pool;
  } else {
    iree_hal_pool_release(backing_pool);
  }
  return status;
}

}  // namespace iree::hal::cts
