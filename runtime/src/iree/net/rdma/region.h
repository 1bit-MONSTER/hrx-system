// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef IREE_NET_RDMA_REGION_H_
#define IREE_NET_RDMA_REGION_H_

#include "iree/async/region.h"
#include "iree/async/slab.h"
#include "iree/net/rdma/context.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

// Registers a CPU-mapped slab once in the context's protection domain. The
// returned async region retains both context and slab, without a proactor.
// Release with iree_async_region_release after all native accesses and caller
// borrows have joined. Wrapped slabs still require caller-owned backing to
// outlive the last region reference.
//
// |address| is the explicit NIC IOVA. Its page offset must match the slab's CPU
// mapping; it need not equal that mapping. Remote keys authorize the registered
// range, not a separately revocable subrange. REMOTE_WRITE requires WRITE.
// Native registration and allocation failures propagate; output is NULL on
// failure. Registration is setup work, never an implicit per-transfer action.
IREE_API_EXPORT iree_status_t iree_net_rdma_region_register_slab(
    iree_net_rdma_context_t* context, iree_async_slab_t* slab, uint64_t address,
    iree_async_buffer_access_flags_t access_flags,
    iree_allocator_t host_allocator, iree_async_region_t** out_region);

// Returns the borrowed context of a region created by this RDMA provider, or
// NULL for other providers. The region keeps the context alive. Admission uses
// this identity once to establish protection-domain compatibility.
IREE_API_EXPORT iree_net_rdma_context_t* iree_net_rdma_region_context(
    const iree_async_region_t* region);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // IREE_NET_RDMA_REGION_H_
