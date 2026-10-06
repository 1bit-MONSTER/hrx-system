// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Copied message compatibility over bounded registered RDMA windows.
//
// The direct endpoint owns native posting, notification credit and retirement.
// This adapter streams CPU-readable sources through registered slots and lends
// received chunks only for the duration of the receive callback. Consequently
// notification credit also authorizes recycling these private receive slots.
// Application-owned direct targets have no such equivalence.

#ifndef IREE_NET_CARRIER_RDMA_CARRIER_H_
#define IREE_NET_CARRIER_RDMA_CARRIER_H_

#include "iree/net/carrier/rdma/direct_endpoint.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

#define IREE_NET_RDMA_MAX_SEND_SPANS 64u

typedef struct iree_net_rdma_carrier_options_t {
  // Logical message admission, independent of the registered slot window.
  uint32_t max_send_operations;
  // Preallocated generated-prefix bytes per message. Larger prefixes use
  // completion-scoped storage, not a smaller logical message limit.
  uint32_t generated_prefix_capacity;
  // Bytes per registered TX/RX slot. Large messages stream across slots.
  uint32_t chunk_capacity;
} iree_net_rdma_carrier_options_t;

// Creates an inactive carrier without asynchronous work. The direct geometry
// requires one entry per write; max_write_operations sizes the TX slot window
// and receive_work_count sizes the RX slot window. Retains context/proactor
// through its direct owner and registers fixed staging storage once.
iree_status_t iree_net_rdma_carrier_create(
    iree_net_rdma_context_t* context, iree_async_proactor_t* proactor,
    iree_net_rdma_connection_control_t* control, uint32_t ordinal,
    uint32_t local_sequence_number,
    iree_net_rdma_direct_endpoint_options_t direct_options,
    iree_net_rdma_carrier_options_t options,
    iree_net_rdma_direct_endpoint_callbacks_t callbacks,
    iree_allocator_t host_allocator, iree_net_carrier_t** out_carrier);

// Borrowed native owner for connection setup, credit and CQ routing. The
// containing connection retains this metadata through its shared CQ join.
// After framed endpoint drains join, join_deactivation on this owner also
// retires any unactivated QP before shared control retirement.
iree_net_rdma_direct_endpoint_t* iree_net_rdma_carrier_direct_endpoint(
    iree_net_carrier_t* carrier);

// Serializes the registered RX extent for the private endpoint-open record.
// Caller storage follows the direct target export capacity contract.
iree_status_t iree_net_rdma_carrier_export_receive(
    iree_net_carrier_t* carrier, iree_byte_span_t data,
    iree_host_size_t* out_length);

// Validates the peer RX geometry/description and connects the underlying QP.
// Called once on the poll owner before the framed view is published.
iree_status_t iree_net_rdma_carrier_connect(iree_net_carrier_t* carrier,
                                            uint32_t remote_queue_number,
                                            uint32_t remote_sequence_number,
                                            uint32_t remote_receive_count,
                                            uint32_t remote_chunk_capacity,
                                            iree_const_byte_span_t target_data);

// Delivers a containing-connection failure on the poll owner. Consumes status.
void iree_net_rdma_carrier_fail(iree_net_carrier_t* carrier,
                                iree_status_t status);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // IREE_NET_CARRIER_RDMA_CARRIER_H_
