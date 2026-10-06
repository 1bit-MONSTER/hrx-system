// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef IREE_NET_CARRIER_RDMA_DIRECT_ENDPOINT_H_
#define IREE_NET_CARRIER_RDMA_DIRECT_ENDPOINT_H_

#include "iree/net/carrier/rdma/connection_control.h"
#include "iree/net/direct_endpoint.h"
#include "iree/net/endpoint_lifecycle.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

typedef struct iree_net_rdma_direct_endpoint_t iree_net_rdma_direct_endpoint_t;

typedef struct iree_net_rdma_direct_endpoint_options_t {
  // Bounded logical writes, separate from the native SQ window.
  uint32_t max_write_operations;
  // Captured source/target entries per logical write, independent of max_sge.
  uint32_t max_write_entries;
  // Enforced outstanding native SQ requests, including unsignaled work.
  uint32_t send_work_count;
  // Empty native RQ entries reserved for placement notifications.
  uint32_t receive_work_count;
  // Maximum linked native requests in one posting call; also scratch size.
  uint32_t post_batch_size;
  // Maximum bytes per native request, additionally limited by the native port.
  // A smaller value changes segmentation, never the logical payload bound.
  uint32_t max_request_length;
  // Native five-bit RNR delay installed before peer traffic.
  uint8_t minimum_rnr_timer;
} iree_net_rdma_direct_endpoint_options_t;

typedef struct iree_net_rdma_direct_endpoint_callbacks_t {
  // Publishes the latest cumulative count of posted notification receives.
  // Poll-owner-only; the containing connection coalesces this private control
  // state independently of data admission and target consumption.
  void (*on_credit)(void* user_data, uint64_t posted_count);
  // Stable containing connection, valid through control's final CQ join.
  void* user_data;
} iree_net_rdma_direct_endpoint_callbacks_t;

// Allocates bounded host metadata and an unconnected native QP. No data
// registration or asynchronous work is created. Control must be ready and its
// CQ capacity must include this endpoint's full send+receive work bound.
// Retains context/proactor; borrows control and optional connection barrier.
// Calls are on the poll owner until the borrowed view is published.
iree_status_t iree_net_rdma_direct_endpoint_create(
    iree_net_rdma_context_t* context, iree_async_proactor_t* proactor,
    iree_net_rdma_connection_control_t* control, uint32_t ordinal,
    uint32_t local_sequence_number,
    iree_net_rdma_direct_endpoint_options_t options,
    iree_net_rdma_direct_endpoint_callbacks_t callbacks,
    iree_net_endpoint_deactivation_barrier_t* connection_barrier,
    iree_allocator_t host_allocator,
    iree_net_rdma_direct_endpoint_t** out_endpoint);

// Native identity advertised by the containing connection during explicit
// endpoint opening. Borrowed for setup; not a serialized native struct.
uint32_t iree_net_rdma_direct_endpoint_queue_number(
    const iree_net_rdma_direct_endpoint_t* endpoint);

// Applies the checked peer OPEN facts to this independent QP. No data work
// begins until activation. Called exactly once before publishing the view.
iree_status_t iree_net_rdma_direct_endpoint_connect(
    iree_net_rdma_direct_endpoint_t* endpoint, uint32_t remote_queue_number,
    uint32_t remote_sequence_number, uint32_t remote_receive_work_count);

// Consumes peer cumulative credit on the poll owner. Validation at this wire
// boundary checks that grants represent receives replenished by actual sends.
iree_status_t iree_net_rdma_direct_endpoint_update_credit(
    iree_net_rdma_direct_endpoint_t* endpoint, uint64_t posted_count);

// Delivers one native CQ entry routed by ordinal, including error entries.
// Metadata remains owned through the shared control CQ's complete join, even
// after the endpoint itself has deactivated and retired native access.
void iree_net_rdma_direct_endpoint_complete(
    iree_net_rdma_direct_endpoint_t* endpoint, const struct ibv_wc* completion);

// Fails admission and begins native access retirement on the poll owner.
// Takes ownership of status; endpoint/connection deactivation joins callbacks.
void iree_net_rdma_direct_endpoint_fail(
    iree_net_rdma_direct_endpoint_t* endpoint, iree_status_t status);

// Joins this endpoint's consumer drain to the containing connection barrier.
// Also retires an unactivated QP; the connection must not deactivate its shared
// control before every independent QP has been retired.
// Created or fully drained owners need no bound connection barrier. An active
// standalone owner must first drain through its direct view instead.
void iree_net_rdma_direct_endpoint_join_deactivation(
    iree_net_rdma_direct_endpoint_t* endpoint);

// Returns a borrowed host view, published only after native peer setup.
iree_net_direct_endpoint_t iree_net_rdma_direct_endpoint_as_direct_endpoint(
    iree_net_rdma_direct_endpoint_t* endpoint);

// Frees a created or fully deactivated endpoint. The containing owner must
// first join its shared CQ so no captured completion still refers to it.
void iree_net_rdma_direct_endpoint_destroy(
    iree_net_rdma_direct_endpoint_t* endpoint);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // IREE_NET_CARRIER_RDMA_DIRECT_ENDPOINT_H_
