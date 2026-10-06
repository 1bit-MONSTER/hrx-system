// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef IREE_NET_CARRIER_RDMA_CONNECTION_H_
#define IREE_NET_CARRIER_RDMA_CONNECTION_H_

#include "iree/net/carrier/rdma/carrier.h"
#include "iree/net/carrier/rdma/completion_queue.h"
#include "iree/net/connection.h"
#include "iree/net/transport_factory.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

typedef struct iree_net_rdma_connection_t iree_net_rdma_connection_t;

// Explicit host geometry. Native resources are allocated only for opened
// endpoints; the shared CQ reserves their full configured error/flush bound.
typedef struct iree_net_rdma_connection_options_t {
  // Combined message/direct ordinal bound, negotiated to the peer minimum.
  uint32_t max_endpoint_count;
  // Local policy for the CQ shared by control and data QPs. Busy polling keeps
  // this connection's caller-owned proactor runnable even when idle; other
  // connections on that proactor still receive bounded service turns.
  iree_net_rdma_completion_queue_mode_t completion_mode;
  // Private control resources, independent of application data backpressure.
  struct {
    // Registered transmit records and enforced control SQ depth.
    uint32_t send_count;
    // Internally replenished control RQ depth.
    uint32_t receive_count;
    // Maximum native records per CQ or CM service visit.
    uint32_t service_batch_size;
    // Native address/route setup timeout in milliseconds, up to INT_MAX.
    uint32_t resolution_timeout_ms;
    // Native five-bit RNR delay encoding; one means 10 us, zero 655.36 ms.
    uint8_t minimum_rnr_timer;
  } control;
  // Native and logical bounds for each registered-placement endpoint.
  iree_net_rdma_direct_endpoint_options_t direct;
  // Copied compatibility path, with independent message and slot geometry.
  struct {
    // Bounded native engine; max_write_entries must be one.
    iree_net_rdma_direct_endpoint_options_t direct;
    // Logical admission, generated-prefix storage and registered slot size.
    iree_net_rdma_carrier_options_t carrier;
  } message;
} iree_net_rdma_connection_options_t;

// Returns host connection defaults. Geometry remains explicitly configurable;
// native and registered slot sizes do not bound logical payload lengths.
iree_net_rdma_connection_options_t iree_net_rdma_connection_options_default(
    void);

// Validates persistent configuration without creating native ownership.
iree_status_t iree_net_rdma_connection_options_validate(
    const iree_net_rdma_connection_options_t* options);

// Creates an inactive connection and its cold control owner. Retains context
// and proactor; failure leaves output NULL. An unstarted owner may be released
// through its base without deactivation. Successful start transfers that
// initial reference into the asynchronous result path below.
iree_status_t iree_net_rdma_connection_create(
    iree_net_rdma_context_t* context, iree_async_proactor_t* proactor,
    const iree_net_rdma_connection_options_t* options,
    iree_allocator_t host_allocator,
    iree_net_rdma_connection_t** out_connection);

// Returns the embedded base without retaining it.
iree_net_connection_t* iree_net_rdma_connection_base(
    iree_net_rdma_connection_t* connection);

// Starts an outbound attempt asynchronously, taking the initial reference.
// The caller holds operation->mutex; binding is installed before owner work.
// Exactly one callback transfers a published connection or reports failure
// after native/callback retirement. Address and callback are captured.
void iree_net_rdma_connection_connect(
    iree_net_rdma_connection_t* connection, const iree_async_address_t* address,
    iree_net_transport_connect_callback_t callback,
    iree_net_transport_connect_operation_t* operation);

// Starts an accepted attempt on the poll owner. Takes the initial reference
// and unconditional ownership of the acknowledged CONNECT_REQUEST ID; private
// data is captured before return. Result delivery is asynchronous as above.
void iree_net_rdma_connection_accept(
    iree_net_rdma_connection_t* connection, struct rdma_cm_id* id,
    iree_const_byte_span_t private_data,
    iree_net_transport_connect_callback_t callback);

// Requests cancellation of an unpublished accepted attempt on its poll owner.
// Its result callback joins cancellation. Outbound callers instead use the
// transport connect operation, which serializes cancellation with publication.
void iree_net_rdma_connection_cancel(iree_net_rdma_connection_t* connection);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // IREE_NET_CARRIER_RDMA_CONNECTION_H_
