// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Framing adapter: wraps a byte-stream carrier to provide message-oriented
// delivery via the message_endpoint interface.
//
// The adapter sits between a raw byte-stream carrier and message protocols by
// adding frame boundary detection. Message-oriented transports expose message
// endpoints directly and do not use this adapter.
//
// ## Zero-copy optimization
//
// When a complete frame consumes the final bytes in a receive buffer, the
// frame is delivered directly with the buffer lease. The handler can move the
// lease to defer processing without copying.
//
// Fragmented frames are reassembled into exact-size host-backed leases. Frames
// preceding other data in one receive buffer and frames delivered through a
// borrowed carrier span are copied into host-backed leases. This keeps
// registered receive buffers available for preposted transport I/O while
// maintaining the message_endpoint contract that the lease is always non-NULL.
//
// ## Ownership model
//
// The adapter takes ownership of the carrier and releases it when freed.
//
// ## Usage
//
// All operations after allocation go through the message_endpoint interface:
//
//   iree_net_framing_adapter_t* adapter = NULL;
//   IREE_RETURN_IF_ERROR(iree_net_framing_adapter_allocate(
//       carrier, frame_length, max_frame_size,
//       /*connection_barrier=*/NULL, allocator, &adapter));
//   iree_net_message_endpoint_t endpoint =
//       iree_net_framing_adapter_as_endpoint(adapter);
//   iree_net_message_endpoint_set_callbacks(endpoint, callbacks);
//   IREE_RETURN_IF_ERROR(iree_net_message_endpoint_activate(endpoint));
//   // ... send/recv via endpoint ...
//   IREE_RETURN_IF_ERROR(iree_net_message_endpoint_deactivate(
//       endpoint, on_deactivated, user_data));
//   // ... after deactivation callback fires ...
//   iree_net_framing_adapter_free(adapter);

#ifndef IREE_NET_FRAMING_ADAPTER_H_
#define IREE_NET_FRAMING_ADAPTER_H_

#include "iree/base/api.h"
#include "iree/net/carrier.h"
#include "iree/net/endpoint_lifecycle.h"
#include "iree/net/frame_accumulator.h"
#include "iree/net/message_endpoint.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

typedef struct iree_net_framing_adapter_t iree_net_framing_adapter_t;

// Allocates a framing adapter over a byte-stream carrier.
//
// The exposed endpoint uses complete wire frames as its messages. Incoming
// stream bytes are accumulated and delivered including their frame headers;
// outgoing messages are forwarded to the carrier unchanged. A
// connection-facing endpoint can layer payload encoding and decoding over this
// adapter when transport framing must be hidden from protocol consumers.
//
// Takes ownership of |carrier| on success and releases it when the adapter is
// freed. On failure the caller retains ownership. The carrier must not be
// activated before passing to this function.
//
// |connection_barrier| optionally binds this adapter to the drain barrier of
// its owning connection. The barrier must be initialized before this call and
// outlive the adapter. Standalone adapters pass NULL.
//
// The |frame_length| callback determines frame boundaries from its exact fixed
// |frame_length.header_size| bytes and rejects malformed headers.
//
// The |max_frame_size| is an admission bound rather than a resident allocation.
// Larger frames report IREE_STATUS_RESOURCE_EXHAUSTED through the endpoint
// error handler.
IREE_API_EXPORT iree_status_t iree_net_framing_adapter_allocate(
    iree_net_carrier_t* carrier, iree_net_frame_length_callback_t frame_length,
    iree_host_size_t max_frame_size,
    iree_net_endpoint_deactivation_barrier_t* connection_barrier,
    iree_allocator_t host_allocator, iree_net_framing_adapter_t** out_adapter);

// Frees the adapter and releases the owned carrier.
//
// The adapter must be deactivated before freeing. Freeing an active adapter is
// a programming error and triggers an assertion failure.
IREE_API_EXPORT void iree_net_framing_adapter_free(
    iree_net_framing_adapter_t* adapter);

// Joins connection deactivation to this adapter's carrier drain.
//
// Starts carrier deactivation when the adapter is ACTIVE, observes an already
// held connection barrier when it is DRAINING, and does nothing when it is
// CREATED or DEACTIVATED. The adapter must have been allocated with its owning
// connection's barrier.
IREE_API_EXPORT void iree_net_framing_adapter_join_deactivation(
    iree_net_framing_adapter_t* adapter);

// Installs transport routing from validated wire headers to logical receive
// queues. Called before activation; router targets outlive native deactivation.
IREE_API_EXPORT void iree_net_framing_adapter_set_receive_router(
    iree_net_framing_adapter_t* adapter,
    iree_net_frame_receive_router_t router);

// Queries the current writable span for a native direct receive. The caller
// writes only that span and commits it through the carrier's on_receive
// callback. Metadata, leased ordinary frames and posted bodies share one frame
// cursor.
IREE_API_EXPORT iree_async_span_t iree_net_framing_adapter_query_receive_buffer(
    iree_net_framing_adapter_t* adapter);

// Returns a borrowed message_endpoint view into this adapter.
//
// The returned endpoint is a lightweight handle (two pointers) that can be
// copied by value. It is valid only while the adapter is alive - there is no
// retain/release. When the adapter is freed, all endpoint references become
// invalid.
//
// All operations (set_callbacks, activate, deactivate, send, query_send_budget)
// are accessed through the returned endpoint's vtable.
IREE_API_EXPORT iree_net_message_endpoint_t
iree_net_framing_adapter_as_endpoint(iree_net_framing_adapter_t* adapter);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // IREE_NET_FRAMING_ADAPTER_H_
