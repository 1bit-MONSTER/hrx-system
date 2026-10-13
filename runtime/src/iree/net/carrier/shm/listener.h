// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef IREE_NET_CARRIER_SHM_LISTENER_H_
#define IREE_NET_CARRIER_SHM_LISTENER_H_

#include "iree/net/carrier/shm/carrier.h"
#include "iree/net/carrier/shm/region.h"
#include "iree/net/transport_factory.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

// Claims a native listener name and admits owner-thread bootstrap work.
// Copies validated geometry/options and the address. Each bounded slot owns
// either a native accept or an in-progress import handshake, never a published
// connection. Stop joins all slots before the listener can be freed.
iree_status_t iree_net_shm_listener_allocate(
    iree_string_view_t address, iree_async_proactor_t* proactor,
    const iree_net_shm_region_layout_t* layout,
    const iree_net_shm_carrier_options_t* carrier_options,
    uint32_t max_pending_connections,
    iree_net_listener_accept_callback_t callback,
    iree_allocator_t host_allocator, iree_net_listener_t** out_listener);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // IREE_NET_CARRIER_SHM_LISTENER_H_
