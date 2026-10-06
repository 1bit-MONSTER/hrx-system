// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef IREE_NET_CARRIER_RDMA_CONNECTION_ROUTE_H_
#define IREE_NET_CARRIER_RDMA_CONNECTION_ROUTE_H_

#include "iree/net/rdma/context.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

// One state-specific native QP configuration returned by the connection
// manager.
typedef struct iree_net_rdma_connection_route_step_t {
  // Resolved native path, policy and negotiated connection QP identity.
  struct ibv_qp_attr attributes;
  // Native fields present in |attributes|.
  int mask;
} iree_net_rdma_connection_route_step_t;

// Cold setup snapshot for independent SEND/WRITE RC QPs after an IB/RoCE peer's
// connection request/response resolves its native route. Contains no owned
// resources, borrowed CM ID, or host progress state. Initial control setup uses
// the negotiated identity; independent data QPs supply their own QPNs and
// packet sequence numbers. The context must outlive every QP using the route.
typedef struct iree_net_rdma_connection_route_t {
  // INIT port and protection-key configuration.
  iree_net_rdma_connection_route_step_t initialize;
  // RTR resolved address vector, MTU, and receive policy.
  iree_net_rdma_connection_route_step_t receive;
  // RTS native retry and timeout policy.
  iree_net_rdma_connection_route_step_t send;
} iree_net_rdma_connection_route_t;

// Captures the route of |id| after CONNECT_REQUEST, CONNECT_RESPONSE, or
// ESTABLISHED, using the context's exact native device. Called on its poll
// owner before connection shutdown begins.
// The snapshot is valid only on success. Native query failures propagate;
// device mismatch returns FAILED_PRECONDITION and non-IB/RoCE devices return
// UNIMPLEMENTED. This does not transfer or retain the ID or its QP.
IREE_API_EXPORT iree_status_t iree_net_rdma_connection_route_initialize(
    iree_net_rdma_context_t* context, struct rdma_cm_id* id,
    iree_net_rdma_connection_route_t* out_route);

// Transitions an independently owned RESET RC QP through INIT/RTR/RTS using a
// captured route from the same context. Queue identities are trusted setup
// values established by the owning connection's peer exchange;
// peer wire validation precedes this call. Neither the QP nor its completion
// state becomes owned by the control CM ID. The caller establishes receive
// readiness before permitting peer data submission. Remote WRITE is enabled;
// READ and atomic resources are not inherited from the control connection.
//
// Native failures propagate and may leave a partially configured QP, which
// its owner must destroy. Even successful control disconnect/destruction does
// not stop this QP: connection teardown explicitly retires all data QPs before
// returning their source/target ownership. No allocation or registration
// occurs.
IREE_API_EXPORT iree_status_t iree_net_rdma_connection_route_connect_queue(
    iree_net_rdma_context_t* context,
    const iree_net_rdma_connection_route_t* route, struct ibv_qp* queue,
    uint32_t local_sequence_number, uint32_t remote_queue_number,
    uint32_t remote_sequence_number);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // IREE_NET_CARRIER_RDMA_CONNECTION_ROUTE_H_
