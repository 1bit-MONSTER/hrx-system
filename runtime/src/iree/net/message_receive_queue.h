// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef IREE_NET_MESSAGE_RECEIVE_QUEUE_H_
#define IREE_NET_MESSAGE_RECEIVE_QUEUE_H_

#include "iree/base/threading/mutex.h"
#include "iree/net/message_receive.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

typedef struct iree_net_endpoint_lifecycle_t iree_net_endpoint_lifecycle_t;

// Transport-owned matching and destination-borrow ownership for one logical
// endpoint. Admission may run on any thread; matching runs on the receive poll
// owner. One shared frame cursor owns the matched body until it completes or
// native receive deactivation returns access. Closing this queue only retires
// unmatched operations; it cannot release a matched native destination.
struct iree_net_message_receive_queue_t {
  // Protects admission, prefix width, matching and terminal state.
  iree_slim_mutex_t mutex;
  // Borrowed endpoint lifetime enclosing the queue and every operation.
  iree_net_endpoint_lifecycle_t* lifecycle;
  // First unmatched receive, with no payload storage owned by this list.
  iree_net_message_receive_operation_t* head;
  // Operation owned by the shared frame cursor, or NULL between bodies.
  iree_net_message_receive_operation_t* active;
  // Common prefix width established by the first accepted receive.
  iree_host_size_t prefix_length;
  // True after endpoint admission closes.
  bool closed;
  // Terminal reason retained until queue deinitialization.
  iree_status_t status;
};

void iree_net_message_receive_queue_initialize(
    iree_net_endpoint_lifecycle_t* lifecycle,
    iree_net_message_receive_queue_t* out_queue);

// Requires actual endpoint retirement, including the active frame borrower.
void iree_net_message_receive_queue_deinitialize(
    iree_net_message_receive_queue_t* queue);

// Captures already-validated public receive parameters and one lifecycle hold.
iree_status_t iree_net_message_receive_queue_submit(
    iree_net_message_receive_queue_t* queue,
    iree_net_message_receive_operation_t* operation,
    const iree_net_message_endpoint_receive_params_t* params);

// Returns the fixed opaque lookahead width; no target is selected by this
// query.
iree_host_size_t iree_net_message_receive_queue_prefix_length(
    iree_net_message_receive_queue_t* queue);

// Claims the matching operation after validating its external body extent.
// Returns NULL for ordinary messages; malformed matched extents fail before
// any destination byte is written.
iree_status_t iree_net_message_receive_queue_match(
    iree_net_message_receive_queue_t* queue, iree_const_byte_span_t prefix,
    iree_host_size_t body_length,
    iree_net_message_receive_operation_t** out_operation);

// Returns an operation after final transport access. Captured callback values
// survive immediate record reuse and its lifecycle hold ends after the
// callback.
void iree_net_message_receive_queue_complete(
    iree_net_message_receive_operation_t* operation, iree_status_t status,
    iree_host_size_t bytes_transferred);

// Closes admission and retires unmatched operations. Consumes |status|. A
// matched operation remains owned by its frame cursor until native retirement.
void iree_net_message_receive_queue_close(
    iree_net_message_receive_queue_t* queue, iree_status_t status);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // IREE_NET_MESSAGE_RECEIVE_QUEUE_H_
