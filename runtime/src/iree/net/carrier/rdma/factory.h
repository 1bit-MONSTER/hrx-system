// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef IREE_NET_CARRIER_RDMA_FACTORY_H_
#define IREE_NET_CARRIER_RDMA_FACTORY_H_

#include "iree/net/carrier/rdma/connection.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

// Bounded host connection and listener resources for one explicit RDMA context.
typedef struct iree_net_rdma_factory_options_t {
  // Geometry independently captured by every connection.
  iree_net_rdma_connection_options_t connection;
  // Maximum simultaneous unpublished incoming connections per listener.
  uint32_t max_pending_connections;
  // Native CM backlog hint, independent of the host handshake bound.
  uint32_t listen_backlog;
} iree_net_rdma_factory_options_t;

static inline iree_net_rdma_factory_options_t
iree_net_rdma_factory_options_default(void) {
  return (iree_net_rdma_factory_options_t){
      /*.connection=*/iree_net_rdma_connection_options_default(),
      /*.max_pending_connections=*/16,
      /*.listen_backlog=*/128,
  };
}

// Creates a factory retaining the caller's explicit native context. Published
// connections and listeners retain their resources independently of the
// factory. Numeric IPv4 host:port and IPv6 [host]:port addresses use native CM
// routing; the selected context device/port must match that route. Resolution
// is async. No hostname resolver, private progress thread or implicit
// registration cache is created. Missing options select defaults.
//
// Message endpoints copy into bounded registered windows and provide retained
// message leases through framing. Direct endpoints use explicit compatible
// registrations without copying payloads. The generic receive_pool arguments
// are unused. Capabilities are RELIABLE | ORDERED within each endpoint; no
// ordering between endpoints or consumer-completion implication is introduced.
IREE_API_EXPORT iree_status_t
iree_net_rdma_factory_create(iree_net_rdma_context_t* context,
                             const iree_net_rdma_factory_options_t* options,
                             iree_allocator_t host_allocator,
                             iree_net_transport_factory_t** out_factory);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // IREE_NET_CARRIER_RDMA_FACTORY_H_
