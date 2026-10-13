// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/net/carrier/shm/connect.h"

#include <string.h>

#include "iree/async/operations/net.h"
#include "iree/async/operations/scheduling.h"
#include "iree/net/carrier/shm/handshake.h"

typedef enum iree_net_shm_connect_phase_e {
  IREE_NET_SHM_CONNECT_PHASE_INITIAL = 0,
  IREE_NET_SHM_CONNECT_PHASE_NATIVE,
  IREE_NET_SHM_CONNECT_PHASE_HANDSHAKE,
  IREE_NET_SHM_CONNECT_PHASE_RESULT,
  IREE_NET_SHM_CONNECT_PHASE_DRAINING_RESULT,
} iree_net_shm_connect_phase_t;

enum iree_net_shm_connect_flag_bits_e {
  IREE_NET_SHM_CONNECT_FLAG_CONTROL_PENDING = 1u << 0,
  IREE_NET_SHM_CONNECT_FLAG_CANCELLATION_REQUESTED = 1u << 1,
  IREE_NET_SHM_CONNECT_FLAG_CANCELLATION_PENDING = 1u << 2,
};
typedef uint32_t iree_net_shm_connect_flags_t;

typedef struct iree_net_shm_connect_state_t {
  // Public cancellation binding, detached before its terminal callback.
  iree_net_transport_connect_operation_t* operation;
  // Retained poll owner for native setup, handshake, and publication.
  iree_async_proactor_t* proactor;
  // Callback phase protected by operation->mutex.
  iree_net_shm_connect_phase_t phase;
  // Independent cancellation/dispatch obligations under operation->mutex.
  iree_net_shm_connect_flags_t flags;
  // Owned terminal result joined across native and control callbacks.
  iree_status_t status;
  // Owned result until public publication, or its cancellation drain completes.
  iree_net_connection_t* connection;
  // Native setup resources, moved into handshake on successful connection.
  iree_net_shm_connection_channel_t channel;
  // Exact resource-import transaction, used only on the poll owner.
  iree_net_shm_handshake_t handshake;
  // Copied client resource limits for validating the server's offer.
  iree_net_shm_region_layout_t limits;
  // Copied process-local send admission configuration.
  iree_net_shm_carrier_options_t carrier_options;
  // Caller result callback; may destroy public operation storage.
  iree_net_transport_connect_callback_t callback;
  // Software handoff reused for initial setup and cancellation.
  iree_async_nop_operation_t control;
#if !defined(IREE_PLATFORM_WINDOWS)
  // Private native connect target, stable through cancellation identity join.
  iree_async_socket_connect_operation_t native_operation;
  // Cancellation receipt independent of the target terminal callback.
  iree_async_cancel_request_t cancellation;
#endif
  // Allocator for this attempt, independent of factory lifetime.
  iree_allocator_t host_allocator;
  // Copied native address string in trailing storage.
  iree_string_view_t address;
} iree_net_shm_connect_state_t;

static void iree_net_shm_connect_state_destroy(
    iree_net_shm_connect_state_t* state) {
  iree_allocator_t host_allocator = state->host_allocator;
  iree_net_shm_connection_channel_deinitialize(&state->channel);
  iree_async_proactor_release(state->proactor);
  iree_allocator_free(host_allocator, state);
}

// Recursion is through the connection's asynchronous deactivation callback;
// a cancelled but ready result must drain before caller storage can be freed.
static void iree_net_shm_connect_finish(iree_net_shm_connect_state_t* state);

static void iree_net_shm_connect_result_drained(void* user_data) {
  iree_net_shm_connect_state_t* state = user_data;
  iree_net_connection_release(state->connection);
  state->connection = NULL;
  iree_slim_mutex_lock(&state->operation->mutex);
  state->phase = IREE_NET_SHM_CONNECT_PHASE_RESULT;
  iree_slim_mutex_unlock(&state->operation->mutex);
  iree_net_shm_connect_finish(state);
}

static void iree_net_shm_connect_finish(iree_net_shm_connect_state_t* state) {
  iree_slim_mutex_lock(&state->operation->mutex);
  if (state->phase != IREE_NET_SHM_CONNECT_PHASE_RESULT ||
      iree_any_bit_set(state->flags,
                       IREE_NET_SHM_CONNECT_FLAG_CONTROL_PENDING |
                           IREE_NET_SHM_CONNECT_FLAG_CANCELLATION_PENDING)) {
    iree_slim_mutex_unlock(&state->operation->mutex);
    return;
  }
  if (iree_status_is_ok(state->status) &&
      iree_any_bit_set(state->flags,
                       IREE_NET_SHM_CONNECT_FLAG_CANCELLATION_REQUESTED)) {
    state->status = iree_status_from_code(IREE_STATUS_CANCELLED);
  }
  if (state->connection && !iree_status_is_ok(state->status)) {
    state->phase = IREE_NET_SHM_CONNECT_PHASE_DRAINING_RESULT;
    iree_slim_mutex_unlock(&state->operation->mutex);
    iree_net_connection_deactivate(
        state->connection, (iree_net_connection_deactivate_callback_t){
                               .fn = iree_net_shm_connect_result_drained,
                               .user_data = state,
                           });
    return;
  }
  state->operation->binding.cancel_fn = NULL;
  state->operation->binding.user_data = NULL;
  iree_status_t status = state->status;
  iree_net_connection_t* connection = state->connection;
  iree_net_transport_connect_callback_t callback = state->callback;
  iree_slim_mutex_unlock(&state->operation->mutex);
  iree_net_shm_connect_state_destroy(state);
  callback.fn(callback.user_data, status, connection);
}

static void iree_net_shm_connect_handshake_complete(
    void* user_data, iree_status_t status, iree_net_connection_t* connection) {
  iree_net_shm_connect_state_t* state = user_data;
  iree_slim_mutex_lock(&state->operation->mutex);
  state->status = iree_status_join(state->status, status);
  state->connection = connection;
  state->phase = IREE_NET_SHM_CONNECT_PHASE_RESULT;
  iree_slim_mutex_unlock(&state->operation->mutex);
  iree_net_shm_connect_finish(state);
}

static void iree_net_shm_connect_begin_handshake(
    iree_net_shm_connect_state_t* state) {
#if defined(IREE_PLATFORM_WINDOWS)
  iree_async_primitive_t primitive = state->channel.pipe;
#else
  iree_async_primitive_t primitive = state->channel.socket->primitive;
#endif
  iree_status_t status = iree_async_local_stream_allocate(
      state->proactor, primitive, IREE_NET_SHM_STORAGE_HANDLE_COUNT,
      state->host_allocator, &state->channel.stream);
  iree_slim_mutex_lock(&state->operation->mutex);
  state->status = iree_status_join(state->status, status);
  state->phase = iree_status_is_ok(state->status)
                     ? IREE_NET_SHM_CONNECT_PHASE_HANDSHAKE
                     : IREE_NET_SHM_CONNECT_PHASE_RESULT;
  bool begin = state->phase == IREE_NET_SHM_CONNECT_PHASE_HANDSHAKE;
  iree_slim_mutex_unlock(&state->operation->mutex);
  if (begin) {
    iree_net_shm_handshake_begin(
        &state->handshake, IREE_NET_SHM_HANDSHAKE_ROLE_CLIENT, state->proactor,
        &state->limits, &state->carrier_options, &state->channel,
        (iree_net_transport_connect_callback_t){
            .fn = iree_net_shm_connect_handshake_complete,
            .user_data = state,
        },
        state->host_allocator);
  } else {
    iree_net_shm_connect_finish(state);
  }
}

#if !defined(IREE_PLATFORM_WINDOWS)
static void iree_net_shm_connect_cancellation_complete(void* user_data) {
  iree_net_shm_connect_state_t* state = user_data;
  iree_slim_mutex_lock(&state->operation->mutex);
  state->flags &= ~IREE_NET_SHM_CONNECT_FLAG_CANCELLATION_PENDING;
  iree_slim_mutex_unlock(&state->operation->mutex);
  iree_net_shm_connect_finish(state);
}

static void iree_net_shm_connect_native_complete(
    void* user_data, iree_async_operation_t* operation, iree_status_t status,
    iree_async_completion_flags_t flags) {
  (void)operation;
  (void)flags;
  iree_net_shm_connect_state_t* state = user_data;
  iree_slim_mutex_lock(&state->operation->mutex);
  state->status = iree_status_join(state->status, status);
  bool begin =
      iree_status_is_ok(state->status) &&
      !iree_any_bit_set(state->flags,
                        IREE_NET_SHM_CONNECT_FLAG_CANCELLATION_REQUESTED);
  state->phase = IREE_NET_SHM_CONNECT_PHASE_RESULT;
  bool cancellation_pending = iree_any_bit_set(
      state->flags, IREE_NET_SHM_CONNECT_FLAG_CANCELLATION_PENDING);
  iree_slim_mutex_unlock(&state->operation->mutex);
  if (cancellation_pending) {
    // Withdrawal may finish the attempt inline. All native target access ends
    // before retiring its identity and reaching the public callback.
    iree_async_proactor_cancel_request_target_retired(state->proactor,
                                                      &state->cancellation);
  } else if (begin) {
    iree_net_shm_connect_begin_handshake(state);
  } else {
    iree_net_shm_connect_finish(state);
  }
}
#endif  // !IREE_PLATFORM_WINDOWS

static void iree_net_shm_connect_control_complete(
    void* user_data, iree_async_operation_t* operation, iree_status_t status,
    iree_async_completion_flags_t flags) {
  (void)operation;
  (void)flags;
  iree_net_shm_connect_state_t* state = user_data;
  iree_slim_mutex_lock(&state->operation->mutex);
  state->flags &= ~IREE_NET_SHM_CONNECT_FLAG_CONTROL_PENDING;
  state->status = iree_status_join(state->status, status);
  bool cancel =
      !iree_status_is_ok(state->status) ||
      iree_any_bit_set(state->flags,
                       IREE_NET_SHM_CONNECT_FLAG_CANCELLATION_REQUESTED);
  if (state->phase == IREE_NET_SHM_CONNECT_PHASE_INITIAL) {
    if (cancel) {
      state->phase = IREE_NET_SHM_CONNECT_PHASE_RESULT;
    } else {
#if defined(IREE_PLATFORM_WINDOWS)
      state->status = iree_async_local_stream_pipe_open(state->address,
                                                        &state->channel.pipe);
      state->phase = IREE_NET_SHM_CONNECT_PHASE_RESULT;
      bool begin = iree_status_is_ok(state->status);
      iree_slim_mutex_unlock(&state->operation->mutex);
      if (begin) {
        iree_net_shm_connect_begin_handshake(state);
      } else {
        iree_net_shm_connect_finish(state);
      }
      return;
#else
      state->status = iree_async_proactor_submit_one(
          state->proactor, &state->native_operation.base);
      state->phase = iree_status_is_ok(state->status)
                         ? IREE_NET_SHM_CONNECT_PHASE_NATIVE
                         : IREE_NET_SHM_CONNECT_PHASE_RESULT;
#endif
    }
  } else if (cancel && state->phase == IREE_NET_SHM_CONNECT_PHASE_HANDSHAKE) {
    iree_slim_mutex_unlock(&state->operation->mutex);
    iree_net_shm_handshake_cancel(&state->handshake);
    return;
#if !defined(IREE_PLATFORM_WINDOWS)
  } else if (cancel && state->phase == IREE_NET_SHM_CONNECT_PHASE_NATIVE) {
    state->flags |= IREE_NET_SHM_CONNECT_FLAG_CANCELLATION_PENDING;
    iree_async_cancel_request_initialize(
        (iree_async_cancel_callback_t){
            .fn = iree_net_shm_connect_cancellation_complete,
            .user_data = state,
        },
        &state->cancellation);
    IREE_CHECK_OK(iree_async_proactor_request_cancel(
        state->proactor, &state->native_operation.base, &state->cancellation));
#endif
  }
  iree_slim_mutex_unlock(&state->operation->mutex);
  iree_net_shm_connect_finish(state);
}

// Called with the public operation mutex held. Native cancellation and helper
// state remain exclusively on the poll owner.
static void iree_net_shm_connect_cancel(void* user_data) {
  iree_net_shm_connect_state_t* state = user_data;
  if (iree_any_bit_set(state->flags,
                       IREE_NET_SHM_CONNECT_FLAG_CANCELLATION_REQUESTED)) {
    return;
  }
  state->flags |= IREE_NET_SHM_CONNECT_FLAG_CANCELLATION_REQUESTED;
  if (iree_any_bit_set(state->flags,
                       IREE_NET_SHM_CONNECT_FLAG_CONTROL_PENDING) ||
      state->phase == IREE_NET_SHM_CONNECT_PHASE_DRAINING_RESULT) {
    return;
  }
  state->flags |= IREE_NET_SHM_CONNECT_FLAG_CONTROL_PENDING;
  iree_async_operation_initialize(&state->control.base,
                                  IREE_ASYNC_OPERATION_TYPE_NOP,
                                  IREE_ASYNC_OPERATION_FLAG_NONE,
                                  iree_net_shm_connect_control_complete, state);
  IREE_CHECK_OK(
      iree_async_proactor_submit_one(state->proactor, &state->control.base));
}

iree_status_t iree_net_shm_connect(
    iree_string_view_t address, iree_async_proactor_t* proactor,
    const iree_net_shm_region_layout_t* limits,
    const iree_net_shm_carrier_options_t* carrier_options,
    iree_net_transport_connect_callback_t callback,
    iree_net_transport_connect_operation_t* operation,
    iree_allocator_t host_allocator) {
  if (!proactor || !address.data || iree_string_view_is_empty(address)) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "SHM connect requires a proactor and address");
  }
#if defined(IREE_PLATFORM_WINDOWS)
  if (!iree_all_bits_set(
          iree_async_proactor_query_capabilities(proactor),
          IREE_ASYNC_PROACTOR_CAPABILITY_WAIT_COMPLETION_PACKET)) {
    return iree_make_status(
        IREE_STATUS_UNAVAILABLE,
        "SHM pipe monitoring requires wait completion packet support");
  }
#else
  iree_async_address_t native_address;
  IREE_RETURN_IF_ERROR(iree_async_address_from_unix(address, &native_address));
#endif
  iree_host_size_t allocation_size = 0;
  IREE_RETURN_IF_ERROR(
      IREE_STRUCT_LAYOUT(sizeof(iree_net_shm_connect_state_t), &allocation_size,
                         IREE_STRUCT_FIELD(address.size, char, NULL)));
  iree_net_shm_connect_state_t* state = NULL;
  IREE_RETURN_IF_ERROR(
      iree_allocator_malloc(host_allocator, allocation_size, (void**)&state));
  memset(state, 0, sizeof(*state));
  state->host_allocator = host_allocator;
  state->operation = operation;
  state->proactor = proactor;
  iree_async_proactor_retain(proactor);
  state->limits = *limits;
  state->carrier_options = *carrier_options;
  state->callback = callback;
  char* address_storage = (char*)(state + 1);
  memcpy(address_storage, address.data, address.size);
  state->address = iree_make_string_view(address_storage, address.size);
  iree_status_t status = iree_ok_status();
#if !defined(IREE_PLATFORM_WINDOWS)
  status = iree_async_socket_create(
      proactor, IREE_ASYNC_SOCKET_TYPE_UNIX_STREAM,
      IREE_ASYNC_SOCKET_OPTION_NONE, &state->channel.socket);
  if (iree_status_is_ok(status)) {
    iree_async_operation_initialize(
        &state->native_operation.base, IREE_ASYNC_OPERATION_TYPE_SOCKET_CONNECT,
        IREE_ASYNC_OPERATION_FLAG_NONE, iree_net_shm_connect_native_complete,
        state);
    state->native_operation.socket = state->channel.socket;
    state->native_operation.address = native_address;
  }
#endif
  if (iree_status_is_ok(status)) {
    operation->binding.cancel_fn = iree_net_shm_connect_cancel;
    operation->binding.user_data = state;
    state->flags = IREE_NET_SHM_CONNECT_FLAG_CONTROL_PENDING;
    iree_async_operation_initialize(
        &state->control.base, IREE_ASYNC_OPERATION_TYPE_NOP,
        IREE_ASYNC_OPERATION_FLAG_NONE, iree_net_shm_connect_control_complete,
        state);
    status = iree_async_proactor_submit_one(proactor, &state->control.base);
  }
  if (!iree_status_is_ok(status)) {
    iree_net_shm_connect_state_destroy(state);
  }
  return status;
}
