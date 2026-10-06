// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Registered placement into peer-advertised target storage. This host interface
// returns source ownership through callbacks on the connection's proactor. It
// is separate from message framing and from device-initiated networking.

#ifndef IREE_NET_DIRECT_ENDPOINT_H_
#define IREE_NET_DIRECT_ENDPOINT_H_

#include "iree/async/span.h"
#include "iree/net/carrier.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

// A checked, borrowed target imported for one connection. Copyable by value
// while that connection and the peer's grant remain alive. Only |length| is
// consumer-readable; owner and native handles are implementation-owned facts
// populated by import, never a caller-constructed target or a wire format.
// Import acquires no remote memory ownership and release implies no revocation.
typedef struct iree_net_direct_target_t {
  // Borrowed connection identity, independent of the endpoint used to import.
  const void* owner;
  // Permitted byte extent, independent of native request limits.
  uint64_t length;
  // Remote permissions established when the description was imported.
  iree_async_buffer_access_flags_t access_flags;
  // Resolved provider facts; no hot-path registry lookup is required.
  union {
    // Remote registration facts for an RDMA connection.
    struct {
      // NIC address of the first permitted byte, not a CPU pointer.
      uint64_t address;
      // Native remote-access key for the containing registration.
      uint32_t key;
    } rdma;
  } handles;
} iree_net_direct_target_t;

// One nonempty registered source range placed into a permitted target range.
typedef struct iree_net_direct_write_entry_t {
  // Registered source, retained by an accepted write through source return.
  iree_async_span_t source;
  // Imported target borrowed only during the write call; its facts are copied.
  const iree_net_direct_target_t* target;
  // Byte offset within the imported target.
  uint64_t target_offset;
} iree_net_direct_write_entry_t;

enum iree_net_direct_write_flag_bits_e {
  IREE_NET_DIRECT_WRITE_FLAG_NONE = 0u,
  // Deliver notification_cookie after this batch's placement is observable
  // by the qualified host target path. This is not target-consumer completion
  // or a causal frontier covering unrelated work.
  IREE_NET_DIRECT_WRITE_FLAG_NOTIFY = 1u << 0,
};
typedef uint32_t iree_net_direct_write_flags_t;

typedef struct iree_net_direct_write_params_t {
  // Optional receiver notification for this explicit batch.
  iree_net_direct_write_flags_t flags;
  // Opaque operation cookie, used only with NOTIFY; not a timeline value.
  uint32_t notification_cookie;
  // Number of nonempty entries in the batch.
  iree_host_size_t entry_count;
  // Temporary descriptors captured before successful admission returns.
  const iree_net_direct_write_entry_t* entries;
  // Exactly one terminal source-return callback for an accepted batch. Success
  // reports the full byte count. Failure reports no committed byte count and
  // may have changed any destination prefix: a batch is not transactional.
  iree_net_send_completion_callback_t completion_callback;
} iree_net_direct_write_params_t;

typedef struct iree_net_direct_endpoint_callbacks_t {
  // Observes this batch's target placement. Does not borrow a native receive
  // buffer; holding the application target cannot consume notification credit.
  // Runs on the owning proactor. An error terminates the endpoint.
  iree_status_t (*on_notification)(void* user_data, uint32_t cookie);
  // Owns the first terminal endpoint failure. Accepted source-return callbacks
  // still follow and may occur after this notification.
  iree_net_carrier_error_fn_t on_error;
  // Borrowed callback context, kept alive through endpoint deactivation.
  void* user_data;
} iree_net_direct_endpoint_callbacks_t;

typedef struct iree_net_direct_endpoint_vtable_t
    iree_net_direct_endpoint_vtable_t;

// Borrowed two-pointer view, owned and drained by its connection. It does not
// retain an endpoint, registration, peer target or connection when copied.
// Activation is externally serialized with other lifecycle calls. Submission,
// budget queries and deactivation are thread-safe; notification and source-
// return callbacks are serialized on the owning proactor. Callback replacement
// happens before activation or on that poll owner.
typedef struct iree_net_direct_endpoint_t {
  // Borrowed implementation owned by the containing connection.
  void* self;
  // Immutable host operations for this implementation.
  const iree_net_direct_endpoint_vtable_t* vtable;
} iree_net_direct_endpoint_t;

struct iree_net_direct_endpoint_vtable_t {
  // Replaces notification/error callbacks as one bundle on the poll owner.
  void (*set_callbacks)(void* self,
                        iree_net_direct_endpoint_callbacks_t callbacks);
  // Activates callback delivery after handlers are installed.
  iree_status_t (*activate)(void* self);
  // Joins native access, accepted work, and callback bodies without waiting.
  iree_status_t (*deactivate)(
      void* self, iree_net_carrier_deactivate_callback_fn_t callback,
      void* user_data);
  // Serializes an explicitly registered local range for the peer.
  iree_status_t (*export_target)(void* self, iree_async_span_t span,
                                 iree_async_buffer_access_flags_t access_flags,
                                 iree_byte_span_t data,
                                 iree_host_size_t* out_length);
  // Parses a peer description once and binds it to this connection.
  iree_status_t (*import_target)(void* self, iree_const_byte_span_t data,
                                 iree_net_direct_target_t* out_target);
  // Reports logical admission capacity, independently of native queue credit.
  iree_net_carrier_send_budget_t (*query_write_budget)(void* self);
  // Captures one batch and returns ownership through its terminal callback.
  iree_status_t (*write)(void* self,
                         const iree_net_direct_write_params_t* params);
};

// Installs both required callbacks before activation or on the poll owner.
// Superseded callback contexts remain valid through deactivation, as with
// message endpoints. No user callback executes from this call.
static inline void iree_net_direct_endpoint_set_callbacks(
    iree_net_direct_endpoint_t endpoint,
    iree_net_direct_endpoint_callbacks_t callbacks) {
  endpoint.vtable->set_callbacks(endpoint.self, callbacks);
}

// Starts receive/notification progress. Failure leaves the endpoint CREATED
// without accepted asynchronous work. Success may race callback delivery.
static inline iree_status_t iree_net_direct_endpoint_activate(
    iree_net_direct_endpoint_t endpoint) {
  return endpoint.vtable->activate(endpoint.self);
}

// Stops new admission and joins native access and all accepted callbacks.
// Successful admission guarantees one optional terminal callback; a rejected
// call invokes none. Completion may run inline if no work remains. This does
// not wait for application consumers or revoke another peer's target grant.
static inline iree_status_t iree_net_direct_endpoint_deactivate(
    iree_net_direct_endpoint_t endpoint,
    iree_net_carrier_deactivate_callback_fn_t callback, void* user_data) {
  return endpoint.vtable->deactivate(endpoint.self, callback, user_data);
}

// Exports reusable target metadata without registering or retaining memory.
// The exporter keeps registration/backing alive through peer access and target
// consumption. |out_length| receives the required wire extent; insufficient
// output capacity returns OUT_OF_RANGE without modifying output bytes.
static inline iree_status_t iree_net_direct_endpoint_export_target(
    iree_net_direct_endpoint_t endpoint, iree_async_span_t span,
    iree_async_buffer_access_flags_t access_flags, iree_byte_span_t data,
    iree_host_size_t* out_length) {
  if (!out_length) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "direct target output length is required");
  }
  *out_length = 0;
  return endpoint.vtable->export_target(endpoint.self, span, access_flags, data,
                                        out_length);
}

// Imports metadata received from this endpoint's peer. The returned value may
// be used by any direct endpoint on this connection while the grant is valid.
// Output is cleared on failure. It does not own the peer's registration.
static inline iree_status_t iree_net_direct_endpoint_import_target(
    iree_net_direct_endpoint_t endpoint, iree_const_byte_span_t data,
    iree_net_direct_target_t* out_target) {
  if (!out_target) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "direct target output is required");
  }
  memset(out_target, 0, sizeof(*out_target));
  return endpoint.vtable->import_target(endpoint.self, data, out_target);
}

// Advisory logical capacity. Zero on a live endpoint means already accepted
// work owns capacity and its terminal callback provides the retry edge. Peer
// receive credit and native SQ occupancy do not consume free logical slots.
static inline iree_net_carrier_send_budget_t
iree_net_direct_endpoint_query_write_budget(
    iree_net_direct_endpoint_t endpoint) {
  return endpoint.vtable->query_write_budget(endpoint.self);
}

// Accepts a bounded metadata batch whose logical extents may require many
// native requests. Descriptors and target values can be reused after return.
// Source registration references are held until the terminal callback begins;
// callers retaining a registration or its bytes beyond that point keep their
// own reference. The callback runs on the owning proactor and may race the
// accepting call's return.
// A rejected call retains nothing and owes no callback. Accepted writes use
// registered storage directly and never silently fall back to payload staging.
// Notifications prove placement of this batch, not target reuse permission or
// completion of unrelated operations. Failure can leave partial target writes.
static inline iree_status_t iree_net_direct_endpoint_write(
    iree_net_direct_endpoint_t endpoint,
    const iree_net_direct_write_params_t* params) {
  if (!params || !params->completion_callback.fn || !params->entry_count ||
      !params->entries ||
      (params->flags & ~IREE_NET_DIRECT_WRITE_FLAG_NOTIFY)) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "invalid direct write parameters");
  }
  return endpoint.vtable->write(endpoint.self, params);
}

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // IREE_NET_DIRECT_ENDPOINT_H_
