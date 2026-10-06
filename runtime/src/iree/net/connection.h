// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Polymorphic connection interface for transport-agnostic endpoint creation.
//
// A connection represents a logical link to a peer. It is produced by a
// successful transport-factory connect or accept operation and owns the
// message and optional direct endpoints opened on that link. Implementations
// decide whether endpoints are independent links or multiplexed streams;
// consumers use the same borrowed endpoint interface in either case.
//
// Lifecycle:
//   - Connection uses create/retain/release pattern.
//   - connect() callback receives connection with ref_count=1.
//   - open_endpoint() returns borrowed-view message endpoints. The connection
//     must outlive all endpoints.
//   - Before releasing, call deactivate() to drain all active carriers. The
//     deactivation callback fires when all carriers are drained and it is safe
//     to release the connection.

#ifndef IREE_NET_CONNECTION_H_
#define IREE_NET_CONNECTION_H_

#include "iree/base/api.h"
#include "iree/base/internal/atomics.h"
#include "iree/net/direct_endpoint.h"
#include "iree/net/message_endpoint.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

//===----------------------------------------------------------------------===//
// iree_net_connection_t
//===----------------------------------------------------------------------===//

// Callback function invoked when an endpoint is ready after open_endpoint().
//
// On success, |status| is OK and |endpoint| is a valid borrowed view into the
// connection's transport stack. The endpoint is valid only while the connection
// is alive. Connection deactivation drains every endpoint before the
// connection can be released.
//
// On failure, |status| contains the error and |endpoint| has self=NULL. The
// callback owns |status| in both cases and must propagate or release it.
typedef void(IREE_API_PTR* iree_net_endpoint_ready_fn_t)(
    void* user_data, iree_status_t status,
    iree_net_message_endpoint_t endpoint);

// Bundled endpoint-ready callback (function pointer + user data).
typedef struct iree_net_endpoint_ready_callback_t {
  // Function invoked when endpoint creation reaches a terminal result.
  iree_net_endpoint_ready_fn_t fn;
  // Opaque value passed to |fn|.
  void* user_data;
} iree_net_endpoint_ready_callback_t;

// Direct counterpart of the message-ready callback. The connection owns the
// borrowed view; failure supplies self=NULL. The callback owns status.
typedef void(IREE_API_PTR* iree_net_direct_endpoint_ready_fn_t)(
    void* user_data, iree_status_t status, iree_net_direct_endpoint_t endpoint);

typedef struct iree_net_direct_endpoint_ready_callback_t {
  // Function invoked after native endpoint setup or terminal failure.
  iree_net_direct_endpoint_ready_fn_t fn;
  // Opaque value passed to fn.
  void* user_data;
} iree_net_direct_endpoint_ready_callback_t;

// Callback function invoked when connection deactivation completes. All
// carriers owned by the connection have been drained and are in the
// DEACTIVATED state.
typedef void(IREE_API_PTR* iree_net_connection_deactivate_fn_t)(
    void* user_data);

// Bundled deactivation callback (function pointer + user data).
typedef struct iree_net_connection_deactivate_callback_t {
  // Function invoked after all connection endpoints have drained.
  iree_net_connection_deactivate_fn_t fn;
  // Opaque value passed to |fn|.
  void* user_data;
} iree_net_connection_deactivate_callback_t;

typedef struct iree_net_connection_t iree_net_connection_t;
typedef struct iree_net_connection_vtable_t iree_net_connection_vtable_t;
typedef struct iree_async_proactor_t iree_async_proactor_t;

// A polymorphic connection to a remote endpoint.
//
// Connections are created by transport factories via connect() or
// accepted by listeners. They provide a unified interface for opening
// message endpoints regardless of the underlying transport.
//
// Concrete transport implementations embed this structure at offset 0.
struct iree_net_connection_t {
  // Reference count controlling connection lifetime.
  iree_atomic_ref_count_t ref_count;
  // Vtable implementing the concrete transport connection.
  const iree_net_connection_vtable_t* vtable;
  // Host allocator used for connection lifetime allocations.
  iree_allocator_t host_allocator;
  // Maximum endpoint slots available on the connection.
  uint32_t max_endpoint_count;
};

struct iree_net_connection_vtable_t {
  // Destroys a fully deactivated connection.
  void (*destroy)(iree_net_connection_t* connection);
  // Begins deactivating all active carriers/endpoints owned by the connection.
  // The callback fires exactly once when all carriers have drained. If no
  // carriers are active, the callback may fire synchronously from this call.
  // After the callback fires, the connection is safe to release.
  //
  // Deactivation is infallible: implementations must pre-allocate any resources
  // needed for drain tracking at connection creation time.
  void (*deactivate)(iree_net_connection_t* connection,
                     iree_net_connection_deactivate_callback_t callback);
  // Begins asynchronously opening one borrowed message endpoint.
  iree_status_t (*open_endpoint)(iree_net_connection_t* connection,
                                 iree_net_endpoint_ready_callback_t callback);
  // Optional registered-placement endpoint. NULL means unsupported; no staged
  // emulation is implied. Shares the message endpoint ordinal namespace.
  iree_status_t (*open_direct_endpoint)(
      iree_net_connection_t* connection,
      iree_net_direct_endpoint_ready_callback_t callback);
  // Returns the proactor that dispatches connection endpoint callbacks.
  // The proactor is borrowed — valid for the connection's lifetime.
  iree_async_proactor_t* (*proactor)(iree_net_connection_t* connection);
};

// Initializes base connection fields. Called by connection implementations.
static inline void iree_net_connection_initialize(
    const iree_net_connection_vtable_t* vtable, iree_allocator_t host_allocator,
    uint32_t max_endpoint_count, iree_net_connection_t* out_connection) {
  IREE_ASSERT_ARGUMENT(vtable);
  IREE_ASSERT_ARGUMENT(out_connection);
  IREE_ASSERT(max_endpoint_count > 0,
              "connection must expose at least one endpoint slot");
  iree_atomic_ref_count_init(&out_connection->ref_count);
  out_connection->vtable = vtable;
  out_connection->host_allocator = host_allocator;
  out_connection->max_endpoint_count = max_endpoint_count;
}

// Retains a reference to the connection (thread-safe).
static inline void iree_net_connection_retain(
    iree_net_connection_t* connection) {
  if (!connection) {
    return;
  }
  iree_atomic_ref_count_inc(&connection->ref_count);
}

// Releases a reference to the connection (thread-safe).
// When the last reference is released, the connection is destroyed.
static inline void iree_net_connection_release(
    iree_net_connection_t* connection) {
  if (!connection) {
    return;
  }
  if (iree_atomic_ref_count_dec(&connection->ref_count) == 1) {
    connection->vtable->destroy(connection);
  }
}

// Begins deactivating all active carriers/endpoints owned by the connection.
//
// This must be called before releasing the connection to ensure all in-flight
// endpoint-ready and transport operations have drained. Without deactivation,
// releasing the connection could free endpoint storage while operations remain
// pending in the proactor's completion queue.
//
// The |callback| fires exactly once when all carriers have transitioned to the
// DEACTIVATED state. If no carriers are active (e.g., endpoints were never
// opened, or the connection was never fully bootstrapped), the callback may
// fire synchronously from this call.
//
// After the callback fires, the connection is safe to release via
// iree_net_connection_release().
//
// Deactivation is infallible: implementations pre-allocate drain tracking
// resources at connection creation time.
static inline void iree_net_connection_deactivate(
    iree_net_connection_t* connection,
    iree_net_connection_deactivate_callback_t callback) {
  IREE_ASSERT_ARGUMENT(callback.fn);
  connection->vtable->deactivate(connection, callback);
}

// Opens a new message endpoint on this connection.
//
// The transport handles multiplexing internally — callers don't need to know
// the underlying transport's multiplexing strategy.
//
// Accepted calls claim monotonically increasing endpoint ordinals. Peers must
// open corresponding endpoint kinds in the same call order; callback
// delivery order does not define endpoint identity.
//
// The |callback| fires exactly once via the proactor when the endpoint is ready
// or creation fails. The callback is always delivered asynchronously, never
// synchronously from this call. On success, the callback receives a borrowed
// endpoint view that is valid for the lifetime of the connection.
//
// Each accepted call allocates an independent endpoint slot. Returns
// RESOURCE_EXHAUSTED if the maximum endpoint count is reached.
static inline iree_status_t iree_net_connection_open_endpoint(
    iree_net_connection_t* connection,
    iree_net_endpoint_ready_callback_t callback) {
  if (!callback.fn) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "endpoint ready callback is required");
  }
  return connection->vtable->open_endpoint(connection, callback);
}

// Opens a borrowed registered-placement endpoint, with the same asynchronous
// readiness and connection-owned drain contract as open_endpoint. Message and
// direct opens share the monotonically claimed ordinal namespace: peers must
// agree on each ordinal's kind. Readiness does not activate the endpoint.
// Unsupported transports return UNIMPLEMENTED synchronously, consume no slot
// and owe no callback. They do not emulate direct placement through staging.
static inline iree_status_t iree_net_connection_open_direct_endpoint(
    iree_net_connection_t* connection,
    iree_net_direct_endpoint_ready_callback_t callback) {
  if (!callback.fn) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "direct endpoint ready callback is required");
  }
  if (!connection->vtable->open_direct_endpoint) {
    return iree_make_status(IREE_STATUS_UNIMPLEMENTED,
                            "transport does not support direct endpoints");
  }
  return connection->vtable->open_direct_endpoint(connection, callback);
}

// Returns the maximum number of endpoint slots available on this connection.
//
// Consumers are responsible for reserving slots for every protocol endpoint
// they intend to open.
static inline uint32_t iree_net_connection_max_endpoint_count(
    iree_net_connection_t* connection) {
  return connection->max_endpoint_count;
}

// Returns the proactor dispatching this connection's async callbacks.
static inline iree_async_proactor_t* iree_net_connection_proactor(
    iree_net_connection_t* connection) {
  return connection->vtable->proactor(connection);
}

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // IREE_NET_CONNECTION_H_
