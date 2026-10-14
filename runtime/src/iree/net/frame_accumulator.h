// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Receives length-prefixed frames from CPU-accessible byte streams.
//
// One shared cursor handles native writes into its current target and incoming
// carrier chunks. Transport codecs resolve a fixed header; an optional router
// selects the logical endpoint's opaque receive matching queue. Matched bodies
// go directly to the posted destination. Ordinary fragmented frames use an
// exact-size movable lease. Maximum frame size is an admission bound, not a
// resident payload allocation.

#ifndef IREE_NET_FRAME_ACCUMULATOR_H_
#define IREE_NET_FRAME_ACCUMULATOR_H_

#include "iree/async/buffer_pool.h"
#include "iree/base/api.h"
#include "iree/net/message_receive.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

//===----------------------------------------------------------------------===//
// Callback types
//===----------------------------------------------------------------------===//

// Resolves the total frame size (header + payload) from available stream data.
//
// The callback receives the complete fixed header and writes the total frame
// size to |out_frame_size|. It rejects malformed headers. The size includes all
// header bytes and cannot exceed the configured maximum. Leaving the size zero
// rejects a header that does not resolve a frame length.
typedef iree_status_t(IREE_API_PTR* iree_net_frame_length_fn_t)(
    void* user_data, iree_const_byte_span_t available,
    iree_host_size_t* out_frame_size);

typedef struct iree_net_frame_length_callback_t {
  // Function that resolves a complete frame length from available bytes.
  iree_net_frame_length_fn_t fn;
  // Opaque value passed to |fn|.
  void* user_data;
  // Exact fixed wire-header size required to resolve the frame length.
  iree_host_size_t header_size;
} iree_net_frame_length_callback_t;

// Routes a structurally validated header to its logical receive queue. NULL
// preserves ordinary complete-frame delivery. The queue belongs to the
// transport and outlives native receive progress, including deactivation.
// This infallible query does not parse protocol payloads or acquire a target.
typedef iree_net_message_receive_queue_t*(
    IREE_API_PTR* iree_net_frame_receive_route_fn_t)(
    void* user_data, iree_const_byte_span_t header);

typedef struct iree_net_frame_receive_router_t {
  // Optional fixed-header routing query.
  iree_net_frame_receive_route_fn_t fn;
  // Transport owner, borrowed through native receive retirement.
  void* user_data;
} iree_net_frame_receive_router_t;

// Called when a complete frame is available for processing.
//
// |frame| includes the frame header. |lease| is non-NULL when ownership of the
// backing storage can be moved for deferred processing. A callback moves the
// lease by copying it and clearing the callback value before returning. When
// |lease| is NULL the frame is borrowed and must be consumed before return.
//
// If the callback returns an error, frame processing stops and the stream is
// terminal. Reset or deinitialize the accumulator before reusing it.
typedef iree_status_t(IREE_API_PTR* iree_net_frame_complete_fn_t)(
    void* user_data, iree_const_byte_span_t frame,
    iree_async_buffer_lease_t* lease);

typedef struct iree_net_frame_complete_callback_t {
  // Function invoked for each complete frame.
  iree_net_frame_complete_fn_t fn;
  // Opaque value passed to |fn|.
  void* user_data;
} iree_net_frame_complete_callback_t;

//===----------------------------------------------------------------------===//
// Frame accumulator
//===----------------------------------------------------------------------===//

// Stream reassembly state with a flexible array for unresolved frame headers.
// Use iree_net_frame_accumulator_calculate_storage_size() to size storage.
typedef struct iree_net_frame_accumulator_t {
  // Callback that determines frame lengths from bounded header prefixes.
  iree_net_frame_length_callback_t frame_length;
  // Callback invoked when a complete frame is available.
  iree_net_frame_complete_callback_t on_frame_complete;
  // Immutable logical endpoint routing installed before native activation.
  iree_net_frame_receive_router_t receive_router;
  // Allocator used for exact-size fragmented frame storage.
  iree_allocator_t host_allocator;
  // Maximum accepted total frame size in bytes.
  iree_host_size_t max_frame_size;
  // Bytes stored while resolving the fixed header and opaque matching prefix.
  iree_host_size_t header_used;
  // Opaque prefix bytes examined after the fixed wire header.
  iree_host_size_t prefix_length;
  // Logical receive queue selected by the fixed header, or NULL.
  iree_net_message_receive_queue_t* receive_queue;
  // Matched destination owned until complete placement or native retirement.
  iree_net_message_receive_operation_t* receive_operation;
  // Total resolved frame size, including transport and matching prefixes.
  iree_host_size_t frame_size;
  // Frame bytes committed to a selected body or ordinary lease.
  iree_host_size_t frame_used;
  // Exact-size storage for an ordinary fragmented frame, never a matched body.
  iree_async_buffer_lease_t frame_lease;
  // Fixed header and bounded opaque prefix, sized by the caller.
  uint8_t header[];
} iree_net_frame_accumulator_t;

// Calculates storage for the fixed header and bounded receive prefix.
static inline iree_status_t iree_net_frame_accumulator_calculate_storage_size(
    iree_host_size_t header_size, iree_host_size_t* out_storage_size) {
  IREE_ASSERT_ARGUMENT(out_storage_size);
  iree_host_size_t lookahead_size = 0;
  if (!iree_host_size_checked_add(header_size,
                                  IREE_NET_MESSAGE_RECEIVE_PREFIX_CAPACITY,
                                  &lookahead_size)) {
    return iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                            "frame lookahead size overflow");
  }
  return IREE_STRUCT_LAYOUT(sizeof(iree_net_frame_accumulator_t),
                            out_storage_size,
                            IREE_STRUCT_FIELD_FAM(lookahead_size, uint8_t));
}

// Initializes an accumulator in preallocated storage.
//
// |accumulator| must reference the size calculated from
// |frame_length.header_size|. |max_frame_size| is an admission bound and
// does not contribute to resident accumulator storage. Fragmented frames are
// allocated from |host_allocator| only after their length is resolved.
IREE_API_EXPORT iree_status_t iree_net_frame_accumulator_initialize(
    iree_net_frame_accumulator_t* accumulator, iree_host_size_t max_frame_size,
    iree_net_frame_length_callback_t frame_length,
    iree_net_frame_complete_callback_t on_frame_complete,
    iree_allocator_t host_allocator);

// Deinitializes after native access retires; returns any active receive
// capture.
IREE_API_EXPORT void iree_net_frame_accumulator_deinitialize(
    iree_net_frame_accumulator_t* accumulator);

// Discards a partial frame after native access retires. An active receive gets
// a CANCELLED receipt before the accumulator returns to its initial state.
IREE_API_EXPORT void iree_net_frame_accumulator_reset(
    iree_net_frame_accumulator_t* accumulator);

// Returns an active receive with |status| after native access retires, then
// discards partial state. Consumes |status|, including when no target is
// active.
IREE_API_EXPORT void iree_net_frame_accumulator_abort(
    iree_net_frame_accumulator_t* accumulator, iree_status_t status);

// Installs immutable logical receive routing before native activation.
IREE_API_EXPORT void iree_net_frame_accumulator_set_receive_router(
    iree_net_frame_accumulator_t* accumulator,
    iree_net_frame_receive_router_t router);

// Returns the next native writable span from trusted cursor state. Native I/O
// writes only into this span, then commits the received subspan through
// push_span. The span's owner survives until that callback or native drain.
IREE_API_EXPORT iree_async_span_t
iree_net_frame_accumulator_query_receive_buffer(
    const iree_net_frame_accumulator_t* accumulator);

// Processes borrowed stream bytes that remain valid only for this call.
//
// Complete frames are delivered with a NULL lease unless they were reassembled
// into accumulator-owned storage. Returns FAILED_PRECONDITION when |data| is
// not CPU-accessible and parser, allocation, or frame callback errors as-is.
IREE_API_EXPORT iree_status_t iree_net_frame_accumulator_push_span(
    iree_net_frame_accumulator_t* accumulator, iree_async_span_t data);

// Processes stream bytes backed by an owned buffer lease.
//
// |data| must be a subspan of |lease->span|. The lease is consumed on every
// return path. A complete final frame may move the input lease without a copy;
// earlier packed frames are borrowed because later bytes still use the lease.
IREE_API_EXPORT iree_status_t iree_net_frame_accumulator_push_lease(
    iree_net_frame_accumulator_t* accumulator, iree_async_span_t data,
    iree_async_buffer_lease_t* lease);

// Returns bytes retained in unresolved-header or fragmented-frame storage.
static inline iree_host_size_t iree_net_frame_accumulator_buffered_bytes(
    const iree_net_frame_accumulator_t* accumulator) {
  return accumulator->header_used + accumulator->frame_used;
}

// Returns true if a partial frame is currently retained.
static inline bool iree_net_frame_accumulator_has_partial_frame(
    const iree_net_frame_accumulator_t* accumulator) {
  return iree_net_frame_accumulator_buffered_bytes(accumulator) > 0;
}

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // IREE_NET_FRAME_ACCUMULATOR_H_
