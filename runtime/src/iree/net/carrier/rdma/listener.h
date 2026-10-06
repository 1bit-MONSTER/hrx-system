// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef IREE_NET_CARRIER_RDMA_LISTENER_H_
#define IREE_NET_CARRIER_RDMA_LISTENER_H_

#include "iree/net/carrier/rdma/connection.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

// Binds a CM listener with validated factory options and numeric address.
// Retains context/proactor and captures geometry. Fixed slots own unpublished
// handshakes only; published connections have no listener dependency. Native
// bind/setup failure unwinds before monitoring begins. Stop closes admission
// immediately and joins native monitoring and unfinished accepts on the poll
// owner before its asynchronous callback authorizes destruction.
iree_status_t iree_net_rdma_listener_create(
    iree_net_rdma_context_t* context, iree_async_proactor_t* proactor,
    const iree_async_address_t* address,
    const iree_net_rdma_connection_options_t* connection_options,
    uint32_t max_pending_connections, uint32_t listen_backlog,
    iree_net_listener_accept_callback_t callback,
    iree_allocator_t host_allocator, iree_net_listener_t** out_listener);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // IREE_NET_CARRIER_RDMA_LISTENER_H_
