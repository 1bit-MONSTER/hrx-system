// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/net/carrier/rdma/connection_route.h"

static iree_status_t iree_net_rdma_connection_route_query_step(
    const iree_net_rdma_library_t* library, struct rdma_cm_id* id,
    enum ibv_qp_state state, iree_net_rdma_connection_route_step_t* out_step) {
  out_step->attributes.qp_state = state;
  if (library->rdma_init_qp_attr(id, &out_step->attributes, &out_step->mask)) {
    int error = errno;
    return iree_make_status(iree_status_code_from_errno(error),
                            "rdma_init_qp_attr(%d): %s", (int)state,
                            strerror(error));
  }
  return iree_ok_status();
}

iree_status_t iree_net_rdma_connection_route_initialize(
    iree_net_rdma_context_t* context, struct rdma_cm_id* id,
    iree_net_rdma_connection_route_t* out_route) {
  memset(out_route, 0, sizeof(*out_route));
  struct ibv_context* device = iree_net_rdma_context_device(context);
  if (id->verbs->device != device->device) {
    return iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                            "RDMA route uses a different native device");
  }
  if (device->device->transport_type != IBV_TRANSPORT_IB) {
    return iree_make_status(
        IREE_STATUS_UNIMPLEMENTED,
        "independent RDMA data QPs require IB/RoCE routing");
  }
  const iree_net_rdma_library_t* library =
      iree_net_rdma_context_library(context);
  IREE_RETURN_IF_ERROR(iree_net_rdma_connection_route_query_step(
      library, id, IBV_QPS_INIT, &out_route->initialize));
  if (out_route->initialize.attributes.port_num !=
      iree_net_rdma_context_port_number(context)) {
    return iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                            "RDMA route uses a different native port");
  }
  IREE_RETURN_IF_ERROR(iree_net_rdma_connection_route_query_step(
      library, id, IBV_QPS_RTR, &out_route->receive));
  IREE_RETURN_IF_ERROR(iree_net_rdma_connection_route_query_step(
      library, id, IBV_QPS_RTS, &out_route->send));
  // The host protocol uses SEND/WRITE, not READ/atomic resources. Keep the
  // resolved identities for initial control setup; independent data queues
  // replace them with their own exchanged identities when applying the route.
  out_route->initialize.attributes.qp_access_flags = IBV_ACCESS_REMOTE_WRITE;
  out_route->receive.attributes.max_dest_rd_atomic = 0;
  out_route->send.attributes.max_rd_atomic = 0;
  return iree_ok_status();
}

static iree_status_t iree_net_rdma_connection_route_apply_step(
    const iree_net_rdma_library_t* library, struct ibv_qp* queue,
    struct ibv_qp_attr* attributes, int mask) {
  int error = library->ibv_modify_qp(queue, attributes, mask);
  if (error) {
    return iree_make_status(iree_status_code_from_errno(error),
                            "ibv_modify_qp(%d): %s", (int)attributes->qp_state,
                            strerror(error));
  }
  return iree_ok_status();
}

iree_status_t iree_net_rdma_connection_route_connect_queue(
    iree_net_rdma_context_t* context,
    const iree_net_rdma_connection_route_t* route, struct ibv_qp* queue,
    uint32_t local_sequence_number, uint32_t remote_queue_number,
    uint32_t remote_sequence_number) {
  const iree_net_rdma_library_t* library =
      iree_net_rdma_context_library(context);
  struct ibv_qp_attr attributes = route->initialize.attributes;
  IREE_RETURN_IF_ERROR(iree_net_rdma_connection_route_apply_step(
      library, queue, &attributes, route->initialize.mask));
  attributes = route->receive.attributes;
  attributes.dest_qp_num = remote_queue_number;
  attributes.rq_psn = remote_sequence_number;
  IREE_RETURN_IF_ERROR(iree_net_rdma_connection_route_apply_step(
      library, queue, &attributes, route->receive.mask));
  attributes = route->send.attributes;
  attributes.sq_psn = local_sequence_number;
  return iree_net_rdma_connection_route_apply_step(library, queue, &attributes,
                                                   route->send.mask);
}
