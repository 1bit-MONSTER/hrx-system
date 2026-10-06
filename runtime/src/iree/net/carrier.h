// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Carrier abstraction: transport-agnostic byte/message movement.
//
// A carrier moves bytes between endpoints. It does not interpret application
// protocols or message semantics; its progress and callbacks are integrated
// with the caller-owned iree_async_proactor_t.
//
// Higher-level message endpoints add framing and multiplexing without changing
// carrier ownership or completion behavior.
//
// ## Composability Rule
//
// Socket and notification work uses proactor operations. Native queue engines
// such as RDMA post and service their queues on the same polling owner through
// bounded event/progress callbacks and explicit handoffs. No carrier installs
// a private worker or a competing host polling loop.
//
// ## Capability-Based Selection
//
// Callers query carrier capabilities to select reliable, ordered, and
// zero-copy paths.
//
// ## Backpressure
//
// Carriers expose message backpressure through query_send_budget(). When local
// operation slots are exhausted, operation completions refresh the budget.

#ifndef IREE_NET_CARRIER_H_
#define IREE_NET_CARRIER_H_

#include <string.h>

#include "iree/async/api.h"
#include "iree/async/buffer_pool.h"
#include "iree/base/api.h"
#include "iree/base/internal/atomics.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

//===----------------------------------------------------------------------===//
// Carrier properties and capabilities
//===----------------------------------------------------------------------===//

// Minimum alignment of storage passed to a send prefix writer.
// This permits protocols to serialize naturally aligned 64-bit fields.
#define IREE_NET_SEND_PREFIX_ALIGNMENT 8

// Carrier lifecycle state for deactivate-before-destroy enforcement.
typedef enum iree_net_carrier_state_e {
  // The carrier has not yet been activated.
  IREE_NET_CARRIER_STATE_CREATED = 0,
  // The receive handler may be receiving data.
  IREE_NET_CARRIER_STATE_ACTIVE = 1,
  // Deactivation is in progress.
  IREE_NET_CARRIER_STATE_DRAINING = 2,
  // All operations are drained and the carrier may be destroyed.
  IREE_NET_CARRIER_STATE_DEACTIVATED = 3,
} iree_net_carrier_state_t;

// Capability flags describing what a carrier can do.
// Query with iree_net_carrier_capabilities().
typedef enum iree_net_carrier_capability_bits_e {
  IREE_NET_CARRIER_CAPABILITY_NONE = 0u,

  // Every accepted send is delivered or completed with a terminal failure;
  // data is never silently dropped.
  IREE_NET_CARRIER_CAPABILITY_RELIABLE = 1u << 0,

  // Successfully delivered data arrives in send order.
  IREE_NET_CARRIER_CAPABILITY_ORDERED = 1u << 1,

  // Carrier supports zero-copy send.
  // The transport can read caller storage directly without staging it.
  IREE_NET_CARRIER_CAPABILITY_ZERO_COPY_TX = 1u << 2,

  // Carrier supports zero-copy receive.
  // Receive handlers can move the delivered lease to retain transport storage.
  IREE_NET_CARRIER_CAPABILITY_ZERO_COPY_RX = 1u << 3,
} iree_net_carrier_capability_bits_t;
typedef uint32_t iree_net_carrier_capabilities_t;

//===----------------------------------------------------------------------===//
// Send/recv parameters
//===----------------------------------------------------------------------===//

// Completion callback for one accepted send operation.
//
// An OK return from iree_net_carrier_send() guarantees exactly one callback.
// The callback runs on the carrier's owning proactor thread and may race with
// the accepting call's return. It marks the point where every buffer referenced
// by the send may be reused. |status| reports the terminal result and transfers
// to the callback, which must propagate or release it. |bytes_transferred| is
// the number of payload bytes accepted by the transport before completion.
typedef void(IREE_API_PTR* iree_net_send_completion_fn_t)(
    void* user_data, iree_status_t status, iree_host_size_t bytes_transferred);

// Completion callback and opaque caller data for one send operation.
typedef struct iree_net_send_completion_callback_t {
  // Function invoked when the send reaches a terminal state.
  iree_net_send_completion_fn_t fn;
  // Opaque value passed to |fn|.
  void* user_data;
} iree_net_send_completion_callback_t;

// Writes one generated prefix into transport-owned admitted storage.
//
// The writer may be invoked synchronously at most once during the send call.
// Implementations may reject the send before invocation when transport storage
// cannot be admitted. Once invoked it runs without transport locks held and
// receives |target| with the exact requested prefix length and at least
// IREE_NET_SEND_PREFIX_ALIGNMENT alignment. Its storage may be consumed
// directly by the transport, such as a shared-memory ring entry or registered
// network buffer. The writer must initialize every byte and must not retain
// |target| after returning. Invocation means the send has been accepted. A
// non-OK return becomes its terminal completion status with zero transferred
// bytes; it is not returned synchronously from the send call.
typedef iree_status_t(IREE_API_PTR* iree_net_send_prefix_write_fn_t)(
    void* user_data, iree_byte_span_t target);

// Description of transient bytes generated synchronously during send.
typedef struct iree_net_send_prefix_t {
  // Exact number of bytes passed to |write|.
  iree_host_size_t length;

  // Function that writes exactly |length| bytes into transport storage.
  iree_net_send_prefix_write_fn_t write;

  // Opaque value passed to |write|.
  void* user_data;
} iree_net_send_prefix_t;

// Returns an empty generated prefix.
static inline iree_net_send_prefix_t iree_net_send_prefix_empty(void) {
  return (iree_net_send_prefix_t){0};
}

// Copies bytes from |user_data| into |target|.
static inline iree_status_t iree_net_send_prefix_copy(void* user_data,
                                                      iree_byte_span_t target) {
  if (!user_data) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "send prefix has null source storage");
  }
  memcpy(target.data, user_data, target.data_length);
  return iree_ok_status();
}

// Returns a generated prefix that synchronously copies |source|.
//
// |source| only needs to remain valid for the duration of the send call.
static inline iree_net_send_prefix_t iree_net_send_prefix_from_bytes(
    iree_const_byte_span_t source) {
  if (iree_const_byte_span_is_empty(source)) {
    return iree_net_send_prefix_empty();
  }
  return (iree_net_send_prefix_t){
      /*.length=*/source.data_length,
      /*.write=*/iree_net_send_prefix_copy,
      /*.user_data=*/(void*)source.data,
  };
}

// Parameters for send operations.
typedef struct iree_net_send_params_t {
  // Transient leading bytes generated before the send call returns.
  iree_net_send_prefix_t generated_prefix;

  // Scatter-gather data borrowed through terminal completion.
  iree_async_span_list_t data;

  // Required callback invoked when the send completes.
  iree_net_send_completion_callback_t completion_callback;
} iree_net_send_params_t;

//===----------------------------------------------------------------------===//
// Backpressure / flow control
//===----------------------------------------------------------------------===//

// Send budget for backpressure management.
// Carriers report both byte budget and operation slot budget because they
// have different limiting factors. Callers check both dimensions before
// submitting operations and retry admission after an outstanding send
// completes.
typedef struct iree_net_carrier_send_budget_t {
  // Bytes available for sending. 0 means backpressured on bandwidth.
  iree_host_size_t bytes;

  // Operation slots available. 0 means backpressured on queue depth.
  uint32_t slots;
} iree_net_carrier_send_budget_t;

//===----------------------------------------------------------------------===//
// Callbacks and handlers
//===----------------------------------------------------------------------===//

// Receive handler invoked when data is received on an activated carrier.
//
// Called from the proactor thread for each received message/chunk. Calls for
// one carrier are serialized in delivery order and never overlap; one call
// returns before the next begins. The handler receives a view into the receive
// buffer and optionally a lease that can be retained if the data is needed
// beyond the callback. An empty span with no lease indicates an orderly peer
// send shutdown (EOF); no later receive data will be delivered. Returning OK
// from EOF preserves the local send direction, while returning an error makes
// the carrier failure terminal in both directions. |lease| may be NULL when
// the storage cannot be retained, including when native receive capacity is
// needed for continued transport progress. In that case |data| is borrowed
// only for this callback; copy bytes that must outlive it. When non-NULL, the
// carrier releases the lease after this callback returns. A handler may take
// ownership by copying the lease and
// clearing the callback's lease value, or may copy the data when holding a
// receive buffer would impede progress. Call iree_async_buffer_lease_release()
// to return a buffer early.
//
// Return iree_ok_status() to indicate successful processing. Returning an
// error causes the carrier to report the error and may trigger deactivation.
typedef iree_status_t(IREE_API_PTR* iree_net_carrier_receive_fn_t)(
    void* user_data, iree_async_span_t data, iree_async_buffer_lease_t* lease);

// Handler invoked for the first terminal carrier error.
//
// The carrier has recorded the error and stopped initiating new receive work
// before this callback fires. The handler takes ownership of |status| and must
// propagate or release it. Outstanding sends still receive their own terminal
// callbacks and may complete after this handler. The handler may begin endpoint
// deactivation to join that drain.
typedef void(IREE_API_PTR* iree_net_carrier_error_fn_t)(void* user_data,
                                                        iree_status_t status);

// Receive and terminal-error handlers installed before carrier activation.
//
// The bundle remains fixed while the carrier is active so received data and
// terminal errors cannot observe different ownership contexts. Endpoint-level
// adapters may independently swap their protocol callbacks.
typedef struct iree_net_carrier_handlers_t {
  // Function invoked for each received byte span.
  iree_net_carrier_receive_fn_t on_receive;
  // Function invoked once for the first terminal carrier error.
  iree_net_carrier_error_fn_t on_error;
  // Opaque value passed to both functions.
  void* user_data;
} iree_net_carrier_handlers_t;

// Callback invoked when carrier deactivation completes (all operations
// drained). After this callback, the carrier is in DEACTIVATED state and safe
// to release.
typedef void(IREE_API_PTR* iree_net_carrier_deactivate_callback_fn_t)(
    void* user_data);

//===----------------------------------------------------------------------===//
// iree_net_carrier_t
//===----------------------------------------------------------------------===//

typedef struct iree_net_carrier_t iree_net_carrier_t;
typedef struct iree_net_carrier_vtable_t iree_net_carrier_vtable_t;

// Carrier implementation vtable.
// See the corresponding iree_net_carrier_*() inline functions for detailed
// documentation of each operation.
struct iree_net_carrier_vtable_t {
  // Destroys a created or fully deactivated carrier.
  void (*destroy)(iree_net_carrier_t* carrier);

  // Activates receive progress on a configured carrier.
  // A non-OK return leaves the carrier in CREATED with no accepted operations.
  iree_status_t (*activate)(iree_net_carrier_t* carrier);
  // Begins infallible deactivation and terminal draining.
  void (*deactivate)(iree_net_carrier_t* carrier,
                     iree_net_carrier_deactivate_callback_fn_t callback,
                     void* user_data);

  // Queries current send admission capacity.
  iree_net_carrier_send_budget_t (*query_send_budget)(
      iree_net_carrier_t* carrier);
  // Submits one callback-completed scatter/gather send.
  iree_status_t (*send)(iree_net_carrier_t* carrier,
                        const iree_net_send_params_t* params);

  // Stops accepting sends and initiates an orderly transport shutdown.
  iree_status_t (*shutdown)(iree_net_carrier_t* carrier);
};

// Base carrier structure. Concrete implementations embed this.
struct iree_net_carrier_t {
  // Reference count controlling carrier lifetime.
  iree_atomic_ref_count_t ref_count;

  // Vtable implementing the concrete carrier.
  const iree_net_carrier_vtable_t* vtable;

  // Lifecycle state updated by carrier implementations.
  iree_atomic_int32_t state;

  // Immutable carrier capabilities set during initialization.
  iree_net_carrier_capabilities_t capabilities;

  // Maximum scatter/gather spans accepted by one send operation.
  iree_host_size_t max_send_spans;

  // Receive and terminal-error handlers installed before activation.
  iree_net_carrier_handlers_t handlers;

  // First terminal carrier status owned until base deinitialization.
  iree_atomic_intptr_t terminal_status;

  // Host allocator used for carrier lifetime allocations.
  iree_allocator_t host_allocator;

  // Number of operations currently in flight (submitted but not completed).
  // Used to determine when it's safe to release the carrier after shutdown.
  iree_atomic_int32_t pending_operations;
};

// Initializes base carrier fields. Called by carrier implementations.
//
// The carrier starts in CREATED state with no handlers. Before activation,
// callers must install handlers via iree_net_carrier_set_handlers().
//
// Parameters:
//   vtable: Implementation vtable.
//   capabilities: Carrier capabilities (RELIABLE, ORDERED, ZERO_COPY_*, etc.).
//   max_send_spans: Maximum scatter-gather spans per operation.
//   host_allocator: Allocator for carrier and internal allocations.
//   out_carrier: Carrier to initialize.
static inline void iree_net_carrier_initialize(
    const iree_net_carrier_vtable_t* vtable,
    iree_net_carrier_capabilities_t capabilities,
    iree_host_size_t max_send_spans, iree_allocator_t host_allocator,
    iree_net_carrier_t* out_carrier) {
  IREE_ASSERT_ARGUMENT(vtable);
  IREE_ASSERT_ARGUMENT(out_carrier);
  IREE_ASSERT(max_send_spans > 0, "carrier must accept at least one send span");
  iree_atomic_ref_count_init(&out_carrier->ref_count);
  out_carrier->vtable = vtable;
  iree_atomic_store(&out_carrier->state, IREE_NET_CARRIER_STATE_CREATED,
                    iree_memory_order_relaxed);
  out_carrier->capabilities = capabilities;
  out_carrier->max_send_spans = max_send_spans;
  out_carrier->handlers = (iree_net_carrier_handlers_t){0};
  iree_atomic_store(&out_carrier->terminal_status, 0,
                    iree_memory_order_relaxed);
  out_carrier->host_allocator = host_allocator;
  iree_atomic_store(&out_carrier->pending_operations, 0,
                    iree_memory_order_relaxed);
}

// Deinitializes shared carrier state before the concrete carrier is freed.
//
// Concrete destroy implementations must call this exactly once after all
// operations and callbacks have drained.
static inline void iree_net_carrier_deinitialize(iree_net_carrier_t* carrier) {
  intptr_t terminal_status = iree_atomic_exchange(&carrier->terminal_status, 0,
                                                  iree_memory_order_acq_rel);
  iree_status_free((iree_status_t)terminal_status);
}

static inline void iree_net_carrier_retain(iree_net_carrier_t* carrier) {
  if (!carrier) {
    return;
  }
  iree_atomic_ref_count_inc(&carrier->ref_count);
}

static inline void iree_net_carrier_release(iree_net_carrier_t* carrier) {
  if (!carrier) {
    return;
  }
  if (iree_atomic_ref_count_dec(&carrier->ref_count) == 1) {
    carrier->vtable->destroy(carrier);
  }
}

// Returns the current lifecycle state of the carrier.
static inline iree_net_carrier_state_t iree_net_carrier_state(
    iree_net_carrier_t* carrier) {
  return (iree_net_carrier_state_t)iree_atomic_load(&carrier->state,
                                                    iree_memory_order_acquire);
}

// Updates the carrier lifecycle state. Called by carrier implementations as
// they transition through CREATED -> ACTIVE -> DRAINING -> DEACTIVATED.
static inline void iree_net_carrier_set_state(iree_net_carrier_t* carrier,
                                              iree_net_carrier_state_t state) {
  iree_atomic_store(&carrier->state, state, iree_memory_order_release);
}

// Atomically transitions |carrier| from |expected_state| to |new_state|.
// Returns true only to the caller that owns the transition.
static inline bool iree_net_carrier_try_transition_state(
    iree_net_carrier_t* carrier, iree_net_carrier_state_t expected_state,
    iree_net_carrier_state_t new_state) {
  int32_t expected_value = (int32_t)expected_state;
  return iree_atomic_compare_exchange_strong(
      &carrier->state, &expected_value, (int32_t)new_state,
      iree_memory_order_acq_rel, iree_memory_order_acquire);
}

// Retires one pending carrier operation. Returns true only to the caller that
// retired the final operation. Retirement must be the caller's final carrier
// access unless it owns another explicit lifetime reference: the final caller
// may complete deactivation and invoke a callback that destroys the carrier.
static inline bool iree_net_carrier_retire_pending_operation(
    iree_net_carrier_t* carrier) {
  int32_t previous = iree_atomic_fetch_sub(&carrier->pending_operations, 1,
                                           iree_memory_order_acq_rel);
  IREE_ASSERT(previous > 0, "carrier retired an operation it did not own");
  return previous == 1;
}

// Installs receive and terminal-error handlers before activation.
//
// Both handlers are required. They share one ownership context and remain
// fixed while the carrier is active. The receive handler is invoked from the
// proactor thread for each received message or chunk. The error handler is
// invoked once if receive processing or the transport fails terminally.
//
// The receive handler receives:
//   - data: View of received bytes (valid only during callback).
//   - lease: If non-NULL, can be retained to keep the buffer beyond callback.
//            Release via iree_async_buffer_lease_release() when done.
//
// Return iree_ok_status() from |handlers.on_receive| to continue receiving.
// Returning an error records a terminal carrier error, invokes
// |handlers.on_error|, and stops receive progress.
static inline iree_status_t iree_net_carrier_set_handlers(
    iree_net_carrier_t* carrier, iree_net_carrier_handlers_t handlers) {
  if (!handlers.on_receive || !handlers.on_error) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "carrier receive and error handlers are required");
  }
  if (iree_net_carrier_state(carrier) != IREE_NET_CARRIER_STATE_CREATED) {
    return iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                            "carrier handlers must be set before activation");
  }
  carrier->handlers = handlers;
  return iree_ok_status();
}

// Returns true when the carrier has recorded a terminal error.
static inline bool iree_net_carrier_has_terminal_error(
    iree_net_carrier_t* carrier) {
  return iree_atomic_load(&carrier->terminal_status,
                          iree_memory_order_acquire) != 0;
}

// Returns an owned clone of the first terminal error, or OK if none exists.
static inline iree_status_t iree_net_carrier_clone_terminal_error(
    iree_net_carrier_t* carrier) {
  intptr_t terminal_status =
      iree_atomic_load(&carrier->terminal_status, iree_memory_order_acquire);
  return terminal_status ? iree_status_clone((iree_status_t)terminal_status)
                         : iree_ok_status();
}

// Records and reports the first terminal carrier error.
//
// Takes ownership of |status|. The first caller stores it for subsequent
// operation failures and transfers an owned clone to the configured error
// handler. Later callers release their duplicate statuses without notifying
// the handler again. Returns true only to the first caller.
//
// The error handler may synchronously begin deactivation. Callers must not
// access the carrier after this function unless they hold an operation or
// lifetime reference that remains valid across that callback.
static inline bool iree_net_carrier_report_terminal_error(
    iree_net_carrier_t* carrier, iree_status_t status) {
  IREE_ASSERT(!iree_status_is_ok(status),
              "terminal carrier error must have a non-OK status");
  if (iree_status_is_ok(status)) {
    return false;
  }

  intptr_t expected_status = 0;
  intptr_t terminal_status = (intptr_t)status;
  if (!iree_atomic_compare_exchange_strong(
          &carrier->terminal_status, &expected_status, terminal_status,
          iree_memory_order_release, iree_memory_order_relaxed)) {
    iree_status_free(status);
    return false;
  }

  IREE_ASSERT(carrier->handlers.on_error,
              "terminal error reported before handlers were installed");
  iree_status_t callback_status = iree_status_clone(status);
  carrier->handlers.on_error(carrier->handlers.user_data, callback_status);
  return true;
}

// Activates the carrier to begin receiving data.
//
// After activation, the carrier auto-posts receives and delivers data to the
// recv handler. The carrier transitions to ACTIVE state.
//
// Prerequisites:
//   - Receive and error handlers must be set via set_handlers().
//   - Carrier must be in CREATED state.
//
// An OK return commits activation and receive callbacks may race with the
// return. A non-OK return is transactional: the carrier remains CREATED, owns
// no submitted operations, and may be retried or released.
static inline iree_status_t iree_net_carrier_activate(
    iree_net_carrier_t* carrier) {
  if (!carrier->handlers.on_receive || !carrier->handlers.on_error) {
    return iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                            "carrier handlers must be set before activation");
  }
  iree_status_t status = carrier->vtable->activate(carrier);
  IREE_ASSERT(iree_status_is_ok(status) || iree_net_carrier_state(carrier) ==
                                               IREE_NET_CARRIER_STATE_CREATED,
              "failed carrier activation must roll back to CREATED");
  return status;
}

// Begins graceful deactivation of the carrier.
//
// This drains outstanding operations and stops auto-posting new receives. The
// carrier transitions through DRAINING -> DEACTIVATED. The callback fires when
// deactivation completes and the carrier is safe to release.
//
// Unlike shutdown() which only stops the send direction, deactivate() stops
// both directions and guarantees all pending operations have completed before
// the callback fires.
//
// Once accepted by the caller's lifecycle state machine, deactivation is
// infallible: transport cleanup failures are reported through the terminal
// error handler and the completion callback still fires after all accepted
// operations reach terminal callbacks.
//
// If no operations remain, the callback may fire synchronously and invalidate
// the carrier owner before this function returns.
//
// Typical sequence:
//   1. iree_net_carrier_deactivate() - begin draining
//   2. Continue polling proactor (operations complete, recv handler may fire)
//   3. Callback fires when all drained - carrier now in DEACTIVATED state
//   4. iree_net_carrier_release() - safe to release
static inline void iree_net_carrier_deactivate(
    iree_net_carrier_t* carrier,
    iree_net_carrier_deactivate_callback_fn_t callback, void* user_data) {
  IREE_ASSERT_ARGUMENT(callback);
  carrier->vtable->deactivate(carrier, callback, user_data);
}

// Returns the capabilities of this carrier.
// O(1) direct field access, no vtable dispatch.
static inline iree_net_carrier_capabilities_t iree_net_carrier_capabilities(
    const iree_net_carrier_t* carrier) {
  return carrier->capabilities;
}

// Returns the maximum caller-provided scatter-gather spans accepted per send.
// A generated prefix does not consume one of these spans.
// O(1) direct field access, no vtable dispatch.
static inline iree_host_size_t iree_net_carrier_max_send_spans(
    const iree_net_carrier_t* carrier) {
  return carrier->max_send_spans;
}

// Returns the number of operations currently owned by the carrier.
//
// This is an implementation and diagnostic query, not a lifecycle
// synchronization primitive. Callers release a carrier only after its
// deactivation callback fires.
static inline int32_t iree_net_carrier_pending_operation_count(
    iree_net_carrier_t* carrier) {
  return iree_atomic_load(&carrier->pending_operations,
                          iree_memory_order_acquire);
}

// Queries the send budget for backpressure management.
//
// When either byte or operation-slot budget reaches zero, the carrier is
// backpressured. A later send completion indicates that callers may query the
// budget and attempt admission again. The result is an advisory snapshot, not
// a reservation; concurrent submissions may consume it before the caller
// submits. The send operation remains the authoritative admission check.
//
// A live carrier must not report zero budget unless capacity is owned by an
// accepted operation. Operation completion provides the retry edge. Terminal
// carriers report a zero budget after delivering their terminal-error callback.
static inline iree_net_carrier_send_budget_t iree_net_carrier_query_send_budget(
    iree_net_carrier_t* carrier) {
  if (iree_net_carrier_has_terminal_error(carrier)) {
    return (iree_net_carrier_send_budget_t){0};
  }
  return carrier->vtable->query_send_budget(carrier);
}

// Submits a send operation.
//
// |params| specifies a generated prefix, borrowed data, and required
// completion callback. The generated prefix followed by |params->data|
// comprises the complete payload. Either part may be empty, but the payload
// must contain at least one byte.
//
// The prefix writer may be invoked synchronously at most once before this call
// returns. Validation and unavailable transport capacity can reject the send
// before invocation. Once invoked it receives transport-owned storage and may
// therefore serialize directly into an SHM ring, registered RDMA staging
// region, or TCP send buffer without caller-side staging. Invocation is the
// observable acceptance point: writer and pre-publication failures complete
// asynchronously with zero transferred bytes instead of being returned from
// this call. A failure after transport publication reports any progress through
// the ordinary completion callback.
//
// The data buffers referenced by |params->data| must remain valid until the
// completion callback fires. This is the standard async I/O contract — the
// proactor may not consume the data during the send() call itself (e.g.,
// io_uring defers SQE processing). Callers that need to send from transient
// storage (stack buffers, temporary allocations) must copy the data into
// stable storage before calling send().
//
// The carrier copies any span descriptors it needs before returning; only the
// referenced byte storage remains caller-owned through completion. The prefix
// writer and its user data are no longer referenced after this function
// returns.
//
// Prerequisites:
//   - Carrier must be activated (ACTIVE state).
//
// Returns INVALID_ARGUMENT for an empty total payload or malformed prefix/span
// list, OUT_OF_RANGE when the caller span count exceeds the carrier limit or
// the total byte count overflows, and the stored terminal status after carrier
// failure. Concrete implementations report lifecycle precondition and
// synchronous admission failures. A non-OK return means the writer was not
// invoked, no capacity remains owned, and the callback will not fire.
static inline iree_status_t iree_net_carrier_send(
    iree_net_carrier_t* carrier, const iree_net_send_params_t* params) {
  if (!params || !params->completion_callback.fn) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "send completion callback is required");
  }
  if ((params->generated_prefix.length == 0) !=
      (params->generated_prefix.write == NULL)) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "send prefix length and writer disagree");
  }
  if (params->generated_prefix.length == 0 &&
      params->generated_prefix.user_data) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "empty send prefix has user data");
  }
  if (params->data.count > 0 && !params->data.values) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "send span list has null storage");
  }
  if (params->data.count > carrier->max_send_spans) {
    return iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                            "send has %" PRIhsz
                            " spans but carrier supports at most %" PRIhsz,
                            params->data.count, carrier->max_send_spans);
  }
  iree_host_size_t total_length = params->generated_prefix.length;
  for (iree_host_size_t i = 0; i < params->data.count; ++i) {
    if (!iree_host_size_checked_add(total_length, params->data.values[i].length,
                                    &total_length)) {
      return iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                              "send payload length overflow");
    }
  }
  if (total_length == 0) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "send payload must be non-empty");
  }
  IREE_RETURN_IF_ERROR(iree_net_carrier_clone_terminal_error(carrier));
  return carrier->vtable->send(carrier, params);
}

// Initiates graceful shutdown of the carrier (send direction only).
//
// After shutdown, no new send operations will succeed. Pending operations
// continue to completion. For TCP, this sends FIN to the peer.
//
// Shutdown is one-way: the carrier can still receive data until the peer also
// closes or the carrier is released. Use deactivate() when done receiving.
static inline iree_status_t iree_net_carrier_shutdown(
    iree_net_carrier_t* carrier) {
  return carrier->vtable->shutdown(carrier);
}

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // IREE_NET_CARRIER_H_
