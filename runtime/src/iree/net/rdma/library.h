// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef IREE_NET_RDMA_LIBRARY_H_
#define IREE_NET_RDMA_LIBRARY_H_

#include <infiniband/verbs.h>  // IWYU pragma: export
#include <rdma/rdma_cma.h>     // IWYU pragma: export

#include "iree/base/api.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

typedef struct iree_dynamic_library_t iree_dynamic_library_t;

// Native symbol lifetime shared by explicit RDMA resource owners. Immutable
// after initialization. Posting, CQ polling and notification arming are inline
// provider dispatches from verbs.h, not separately loaded symbols.
typedef struct iree_net_rdma_library_t {
  // Loaded libibverbs, retained until all provider resources have retired.
  iree_dynamic_library_t* verbs_library;
  // Wrapper for process-resident librdmacm and its canonical device inventory.
  iree_dynamic_library_t* cm_library;

  // Native entry points used by device, memory and queue resource owners.
#define IREE_NET_RDMA_SYMBOL(library, result, name, arguments) \
  result(*name) arguments;
#include "iree/net/rdma/library_symbols.h"  // IWYU pragma: export
} iree_net_rdma_library_t;

// Loads the canonical Linux rdma-core libraries using the platform library
// search path. Failure leaves a deinitialized object. CM and its verbs
// dependency remain resident because CM owns a process-lifetime live-device
// inventory. No IREE process-global cache is installed; every native resource
// must keep its symbol-table owner alive.
IREE_API_EXPORT iree_status_t iree_net_rdma_library_initialize(
    iree_allocator_t host_allocator, iree_net_rdma_library_t* out_library);

// Releases library wrappers after their owned native resources are destroyed.
// The native CM module and its canonical live-device inventory remain resident.
// Safe on a zero-initialized or unsuccessfully initialized object.
IREE_API_EXPORT void iree_net_rdma_library_deinitialize(
    iree_net_rdma_library_t* library);

// Queries the full port attributes using the provider's extended entry point
// when present, matching the verbs.h wrapper without a link-time dependency on
// its compatibility symbol. Returns zero on success or a native error number.
IREE_API_EXPORT int iree_net_rdma_library_query_port(
    const iree_net_rdma_library_t* library, struct ibv_context* context,
    uint8_t port_number, struct ibv_port_attr* out_attributes);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // IREE_NET_RDMA_LIBRARY_H_
