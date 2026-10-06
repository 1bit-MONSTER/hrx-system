// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/net/rdma/region.h"

typedef struct iree_net_rdma_region_t {
  // Shared async registration interface, including retained slab and native MR.
  iree_async_region_t base;
  // Allocator used for this object.
  iree_allocator_t host_allocator;
  // Retained native protection-domain and library owner.
  iree_net_rdma_context_t* context;
} iree_net_rdma_region_t;

static void iree_net_rdma_region_destroy(iree_async_region_t* base_region) {
  iree_net_rdma_region_t* region = (iree_net_rdma_region_t*)base_region;
  const iree_net_rdma_library_t* library =
      iree_net_rdma_context_library(region->context);
  int error = library->ibv_dereg_mr(base_region->handles.rdma.mr);
  if (error) {
    iree_status_abort(iree_make_status(iree_status_code_from_errno(error),
                                       "ibv_dereg_mr: %s", strerror(error)));
  }
  iree_async_slab_release(base_region->slab);
  iree_net_rdma_context_release(region->context);
  iree_allocator_free(region->host_allocator, region);
}

iree_status_t iree_net_rdma_region_register_slab(
    iree_net_rdma_context_t* context, iree_async_slab_t* slab, uint64_t address,
    iree_async_buffer_access_flags_t access_flags,
    iree_allocator_t host_allocator, iree_async_region_t** out_region) {
  *out_region = NULL;
  const iree_async_buffer_access_flags_t supported_flags =
      IREE_ASYNC_BUFFER_ACCESS_FLAG_READ | IREE_ASYNC_BUFFER_ACCESS_FLAG_WRITE |
      IREE_ASYNC_BUFFER_ACCESS_FLAG_REMOTE_READ |
      IREE_ASYNC_BUFFER_ACCESS_FLAG_REMOTE_WRITE;
  if (!context || !slab || !slab->base_ptr || !slab->total_size ||
      slab->buffer_count > UINT32_MAX || (access_flags & ~supported_flags)) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "RDMA registration requires a context, nonempty "
                            "CPU-mapped slab and supported access flags");
  }
  if ((access_flags & IREE_ASYNC_BUFFER_ACCESS_FLAG_REMOTE_WRITE) &&
      !(access_flags & IREE_ASYNC_BUFFER_ACCESS_FLAG_WRITE)) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "remote write registration requires local write");
  }
  if (slab->total_size - 1 > UINT64_MAX - address) {
    return iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                            "RDMA registration exceeds the NIC address space");
  }
  unsigned int native_access = 0;
  if (access_flags & IREE_ASYNC_BUFFER_ACCESS_FLAG_WRITE) {
    native_access |= IBV_ACCESS_LOCAL_WRITE;
  }
  if (access_flags & IREE_ASYNC_BUFFER_ACCESS_FLAG_REMOTE_READ) {
    native_access |= IBV_ACCESS_REMOTE_READ;
  }
  if (access_flags & IREE_ASYNC_BUFFER_ACCESS_FLAG_REMOTE_WRITE) {
    native_access |= IBV_ACCESS_REMOTE_WRITE;
  }
  iree_net_rdma_region_t* region = NULL;
  IREE_RETURN_IF_ERROR(
      iree_allocator_malloc(host_allocator, sizeof(*region), (void**)&region));
  const iree_net_rdma_library_t* library =
      iree_net_rdma_context_library(context);
  struct ibv_mr* memory_region = library->ibv_reg_mr_iova2(
      iree_net_rdma_context_protection_domain(context), slab->base_ptr,
      slab->total_size, address, native_access);
  iree_status_t status = iree_ok_status();
  if (!memory_region) {
    int error = errno;
    status = iree_make_status(iree_status_code_from_errno(error),
                              "ibv_reg_mr_iova2: %s", strerror(error));
  }
  if (iree_status_is_ok(status)) {
    iree_atomic_ref_count_init(&region->base.ref_count);
    region->host_allocator = host_allocator;
    region->context = context;
    iree_net_rdma_context_retain(context);
    region->base.slab = slab;
    iree_async_slab_retain(slab);
    region->base.destroy_fn = iree_net_rdma_region_destroy;
    region->base.type = IREE_ASYNC_REGION_TYPE_RDMA;
    region->base.access_flags = access_flags;
    region->base.base_ptr = slab->base_ptr;
    region->base.length = slab->total_size;
    region->base.buffer_size = slab->buffer_size;
    region->base.buffer_count = (uint32_t)slab->buffer_count;
    region->base.handles.rdma.address = address;
    region->base.handles.rdma.lkey = memory_region->lkey;
    region->base.handles.rdma.rkey = memory_region->rkey;
    region->base.handles.rdma.mr = memory_region;
    *out_region = &region->base;
  } else {
    iree_allocator_free(host_allocator, region);
  }
  return status;
}

iree_net_rdma_context_t* iree_net_rdma_region_context(
    const iree_async_region_t* region) {
  return region && region->destroy_fn == iree_net_rdma_region_destroy
             ? ((const iree_net_rdma_region_t*)region)->context
             : NULL;
}
