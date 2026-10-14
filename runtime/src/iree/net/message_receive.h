// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef IREE_NET_MESSAGE_RECEIVE_H_
#define IREE_NET_MESSAGE_RECEIVE_H_

#include "iree/base/api.h"
#include "iree/base/internal/atomics.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

// Maximum opaque matching prefix captured by a posted message receive. Prefix
// storage is metadata, independent of the target's payload capacity.
#define IREE_NET_MESSAGE_RECEIVE_PREFIX_CAPACITY 64u

// Returns the destination borrow after complete placement or actual transport
// retirement. Success means the complete target range was written. On failure,
// |bytes_transferred| reports the body bytes placed before retirement; those
// bytes are not a ready message. The matching prefix is excluded from the
// count.
//
// Successful placement completes on the receive poll owner. Cancellation of an
// unmatched operation may complete synchronously on the deactivation caller.
// The callback consumes |status| and may reuse the operation immediately.
typedef void(IREE_API_PTR* iree_net_receive_completion_fn_t)(
    void* user_data, iree_status_t status, iree_host_size_t bytes_transferred);

typedef struct iree_net_receive_completion_callback_t {
  // Required terminal destination-return callback.
  iree_net_receive_completion_fn_t fn;
  // Caller-owned callback target, valid through callback return.
  void* user_data;
} iree_net_receive_completion_callback_t;

typedef struct iree_net_message_receive_queue_t
    iree_net_message_receive_queue_t;

// Caller-owned capture for one posted message receive. Zero-initialize before
// first use. The caller keeps this storage and its destination alive through
// terminal completion, including after an endpoint error. Admission captures
// the matching prefix and callback; it creates no payload allocation or native
// registration. Reuse is permitted from the terminal callback onward.
//
// All fields are implementation-owned while admitted.
typedef struct iree_net_message_receive_operation_t {
  // Nonzero while an endpoint owns this operation; zero is reusable.
  iree_atomic_int32_t claimed;
  // Receive admission owner, valid until the terminal callback begins.
  iree_net_message_receive_queue_t* queue;
  // Next unmatched operation in the owner's admission list.
  struct iree_net_message_receive_operation_t* next;
  // Exact current destination borrowed through terminal completion.
  iree_byte_span_t data;
  // Callback captured during admission.
  iree_net_receive_completion_callback_t completion_callback;
  // Opaque prefix copied during admission, excluding transport framing.
  uint8_t prefix[IREE_NET_MESSAGE_RECEIVE_PREFIX_CAPACITY];
} iree_net_message_receive_operation_t;

// Parameters captured by one connection-facing message endpoint receive.
typedef struct iree_net_message_endpoint_receive_params_t {
  // Exact leading message bytes used for matching. Borrowed only for the call.
  iree_const_byte_span_t prefix;
  // Writable message body after the prefix, borrowed until terminal completion.
  iree_byte_span_t data;
  // Required callback returning the destination borrow.
  iree_net_receive_completion_callback_t completion_callback;
} iree_net_message_endpoint_receive_params_t;

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // IREE_NET_MESSAGE_RECEIVE_H_
