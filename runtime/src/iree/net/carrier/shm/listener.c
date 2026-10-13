// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/net/carrier/shm/listener.h"

#include <string.h>
#if !defined(IREE_PLATFORM_WINDOWS)
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "iree/async/operations/net.h"
#include "iree/async/operations/scheduling.h"
#include "iree/net/carrier/shm/handshake.h"

typedef struct iree_net_shm_listener_t iree_net_shm_listener_t;

typedef enum iree_net_shm_listener_state_e {
  IREE_NET_SHM_LISTENER_STATE_LISTENING = 0,
  IREE_NET_SHM_LISTENER_STATE_FAILED,
  IREE_NET_SHM_LISTENER_STATE_STOPPING,
  IREE_NET_SHM_LISTENER_STATE_STOPPED,
} iree_net_shm_listener_state_t;

typedef enum iree_net_shm_listener_slot_phase_e {
  IREE_NET_SHM_LISTENER_SLOT_IDLE = 0,
  IREE_NET_SHM_LISTENER_SLOT_ACCEPTING,
  IREE_NET_SHM_LISTENER_SLOT_HANDSHAKING,
  IREE_NET_SHM_LISTENER_SLOT_DRAINING_SETUP,
  IREE_NET_SHM_LISTENER_SLOT_DRAINING_RESULT,
} iree_net_shm_listener_slot_phase_t;

enum iree_net_shm_listener_slot_flag_bits_e {
  IREE_NET_SHM_LISTENER_SLOT_FLAG_NATIVE_PENDING = 1u << 0,
  IREE_NET_SHM_LISTENER_SLOT_FLAG_CANCELLATION_PENDING = 1u << 1,
};
typedef uint32_t iree_net_shm_listener_slot_flags_t;

typedef struct iree_net_shm_listener_slot_t {
  // Containing listener, alive until this slot has drained.
  iree_net_shm_listener_t* listener;
  // Current ownership phase, accessed exclusively on the poll owner.
  iree_net_shm_listener_slot_phase_t phase;
  // Native accept target/receipt obligations on POSIX.
  iree_net_shm_listener_slot_flags_t flags;
  // Native setup channel moved into the handshake after acceptance.
  iree_net_shm_connection_channel_t channel;
  // Resource-import transaction while this slot is handshaking.
  iree_net_shm_handshake_t handshake;
  // Ready connection privately drained if stop wins public acceptance.
  iree_net_connection_t* connection;
  // Native setup failure held until the channel can be safely closed.
  iree_status_t status;
#if !defined(IREE_PLATFORM_WINDOWS)
  // Single-shot accept, not rearmed before its identity receipt retires.
  iree_async_socket_accept_operation_t accept;
  // Independent native cancellation ownership.
  iree_async_cancel_request_t cancellation;
#endif
} iree_net_shm_listener_slot_t;

struct iree_net_shm_listener_t {
  // Public listener base; must be first.
  iree_net_listener_t base;
  // Protects off-thread stop admission and owner control accounting.
  iree_slim_mutex_t mutex;
  // Retained poll owner for every native accept/handshake callback.
  iree_async_proactor_t* proactor;
  // Listener admission/lifetime phase, protected by |mutex|.
  iree_net_shm_listener_state_t state;
  // Number of queued/executing control callbacks, including user callbacks.
  uint32_t pending_control_count;
  // Whether |control| is queued; protected by |mutex|.
  bool control_queued;
  // Reusable normal handoff for kickoff, terminal native error, and stop.
  iree_async_nop_operation_t control;
  // Immutable geometry offered to each accepted peer.
  iree_net_shm_region_layout_t layout;
  // Immutable per-endpoint process-local admission bounds.
  iree_net_shm_carrier_options_t carrier_options;
  // Bound on simultaneous native accepts and import handshakes.
  uint32_t slot_count;
  // Preallocated ownership slots in trailing storage.
  iree_net_shm_listener_slot_t* slots;
  // Copied native name with a trailing NUL for filesystem operations.
  iree_string_view_t address;
  // Receives fully usable connections and terminal setup errors.
  iree_net_listener_accept_callback_t accept_callback;
  // Receives the exactly-once complete listener drain.
  iree_net_listener_stopped_callback_t stopped_callback;
#if defined(IREE_PLATFORM_WINDOWS)
  // Duplicate of a real pipe instance, not an unserviced extra instance.
  // Bridges failed-handshake close/rearm so FIRST_INSTANCE ownership cannot
  // lapse. Replaced by each new accept pipe before public result callbacks.
  iree_async_primitive_t name_guard;
#else
  // Owned bound socket, live through all accept identity receipts.
  iree_async_socket_t* socket;
  // Identity of this listener's pathname entry, absent for abstract addresses.
  struct {
    // Whether the original filesystem entry is still owned by this listener.
    bool owned;
    // Filesystem device containing the original socket entry.
    dev_t device;
    // Inode of the original socket entry, never a replacement occupant.
    ino_t inode;
  } path;
#endif
  // Allocator for this object and every accepted connection it constructs.
  iree_allocator_t host_allocator;
};

static bool iree_net_shm_listener_is_listening(
    iree_net_shm_listener_t* listener) {
  iree_slim_mutex_lock(&listener->mutex);
  bool listening = listener->state == IREE_NET_SHM_LISTENER_STATE_LISTENING;
  iree_slim_mutex_unlock(&listener->mutex);
  return listening;
}

static iree_status_t iree_net_shm_listener_release_name(
    iree_net_shm_listener_t* listener) {
#if defined(IREE_PLATFORM_WINDOWS)
  iree_async_primitive_close(&listener->name_guard);
#else
  if (listener->path.owned) {
    listener->path.owned = false;
    struct stat metadata;
    if (lstat(listener->address.data, &metadata) != 0) {
      int error = errno;
      if (error != ENOENT) {
        return iree_make_status(iree_status_code_from_errno(error),
                                "inspecting SHM listener path during close");
      }
    } else if (metadata.st_dev == listener->path.device &&
               metadata.st_ino == listener->path.inode) {
      if (unlink(listener->address.data) != 0 && errno != ENOENT) {
        return iree_make_status(iree_status_code_from_errno(errno),
                                "unlinking SHM listener path");
      }
    }
  }
#endif
  return iree_ok_status();
}

static void iree_net_shm_listener_finish_stop(
    iree_net_shm_listener_t* listener) {
  // Slots have a single poll owner; only the admission state needs the mutex.
  for (uint32_t i = 0; i < listener->slot_count; ++i) {
    if (listener->slots[i].phase != IREE_NET_SHM_LISTENER_SLOT_IDLE) {
      return;
    }
  }
  iree_net_listener_stopped_callback_t callback = {0};
  iree_slim_mutex_lock(&listener->mutex);
  if (listener->state == IREE_NET_SHM_LISTENER_STATE_STOPPING &&
      listener->pending_control_count == 0) {
    listener->state = IREE_NET_SHM_LISTENER_STATE_STOPPED;
    callback = listener->stopped_callback;
  }
  iree_slim_mutex_unlock(&listener->mutex);
  if (callback.fn) {
    callback.fn(callback.user_data);
  }
}

static void iree_net_shm_listener_kick_locked(
    iree_net_shm_listener_t* listener) {
  if (listener->control_queued) {
    return;
  }
  listener->control_queued = true;
  ++listener->pending_control_count;
  iree_async_operation_t* operation = &listener->control.base;
  iree_async_operation_initialize(operation, IREE_ASYNC_OPERATION_TYPE_NOP,
                                  IREE_ASYNC_OPERATION_FLAG_NONE,
                                  operation->completion_fn, listener);
  IREE_CHECK_OK(iree_async_proactor_submit_one(listener->proactor, operation));
}

// Native listener/setup errors stop acceptance without inventing retries.
// Peer-specific handshake failures instead retire/replenish only their slot.
static void iree_net_shm_listener_fail(iree_net_shm_listener_t* listener,
                                       iree_status_t status) {
  iree_slim_mutex_lock(&listener->mutex);
  if (listener->state == IREE_NET_SHM_LISTENER_STATE_LISTENING) {
    listener->state = IREE_NET_SHM_LISTENER_STATE_FAILED;
    iree_net_shm_listener_kick_locked(listener);
  }
  iree_slim_mutex_unlock(&listener->mutex);
  listener->accept_callback.fn(listener->accept_callback.user_data, status,
                               NULL);
}

// Rearming follows the preceding slot callback, creating an asynchronous cycle
// through native acceptance and handshake completion.
static void iree_net_shm_listener_start_slot(
    iree_net_shm_listener_slot_t* slot);

static void iree_net_shm_listener_result_drained(void* user_data) {
  iree_net_shm_listener_slot_t* slot = user_data;
  iree_net_connection_release(slot->connection);
  slot->connection = NULL;
  slot->phase = IREE_NET_SHM_LISTENER_SLOT_IDLE;
  iree_net_shm_listener_finish_stop(slot->listener);
}

static void iree_net_shm_listener_handshake_complete(
    void* user_data, iree_status_t status, iree_net_connection_t* connection) {
  iree_net_shm_listener_slot_t* slot = user_data;
  iree_net_shm_listener_t* listener = slot->listener;
  if (!iree_net_shm_listener_is_listening(listener)) {
    // Stop owns these accepted peers and reports no spurious accept failure
    // for its own cancellation. Any native/peer failure remains diagnostic.
    if (iree_status_is_cancelled(status)) {
      iree_status_free(status);
    } else if (!iree_status_is_ok(status)) {
      listener->accept_callback.fn(listener->accept_callback.user_data, status,
                                   NULL);
    }
    if (connection) {
      slot->connection = connection;
      slot->phase = IREE_NET_SHM_LISTENER_SLOT_DRAINING_RESULT;
      iree_net_connection_deactivate(
          connection, (iree_net_connection_deactivate_callback_t){
                          .fn = iree_net_shm_listener_result_drained,
                          .user_data = slot,
                      });
    } else {
      slot->phase = IREE_NET_SHM_LISTENER_SLOT_IDLE;
      iree_net_shm_listener_finish_stop(listener);
    }
    return;
  }
  // Replenish native capacity (and move the Windows name guard) before exposing
  // a connection whose user may immediately deactivate it in this callback.
  slot->phase = IREE_NET_SHM_LISTENER_SLOT_IDLE;
  iree_net_shm_listener_start_slot(slot);
  listener->accept_callback.fn(listener->accept_callback.user_data, status,
                               connection);
  iree_net_shm_listener_finish_stop(listener);
}

static void iree_net_shm_listener_setup_drained(void* user_data) {
  iree_net_shm_listener_slot_t* slot = user_data;
  iree_net_shm_listener_t* listener = slot->listener;
  iree_net_shm_connection_channel_deinitialize(&slot->channel);
  slot->phase = IREE_NET_SHM_LISTENER_SLOT_IDLE;
  iree_status_t status = slot->status;
  slot->status = iree_ok_status();
  if (!iree_status_is_ok(status)) {
    iree_net_shm_listener_fail(listener, status);
  }
  iree_net_shm_listener_finish_stop(listener);
}

static void iree_net_shm_listener_accept_done(
    iree_net_shm_listener_slot_t* slot) {
  iree_net_shm_listener_t* listener = slot->listener;
  bool listening = iree_net_shm_listener_is_listening(listener);
  if (iree_status_is_ok(slot->status) && listening) {
#if !defined(IREE_PLATFORM_WINDOWS)
    slot->status = iree_async_local_stream_allocate(
        listener->proactor, slot->channel.socket->primitive,
        IREE_NET_SHM_STORAGE_HANDLE_COUNT, listener->host_allocator,
        &slot->channel.stream);
#endif
    if (iree_status_is_ok(slot->status)) {
      slot->phase = IREE_NET_SHM_LISTENER_SLOT_HANDSHAKING;
      iree_net_shm_handshake_begin(
          &slot->handshake, IREE_NET_SHM_HANDSHAKE_ROLE_SERVER,
          listener->proactor, &listener->layout, &listener->carrier_options,
          &slot->channel,
          (iree_net_transport_connect_callback_t){
              .fn = iree_net_shm_listener_handshake_complete,
              .user_data = slot,
          },
          listener->host_allocator);
      return;
    }
  }
  if (!listening && iree_status_is_cancelled(slot->status)) {
    iree_status_free(slot->status);
    slot->status = iree_ok_status();
  }
  slot->phase = IREE_NET_SHM_LISTENER_SLOT_DRAINING_SETUP;
  if (slot->channel.stream) {
    IREE_CHECK_OK(iree_async_local_stream_deactivate(
        slot->channel.stream, (iree_async_local_stream_deactivated_callback_t){
                                  .fn = iree_net_shm_listener_setup_drained,
                                  .user_data = slot,
                              }));
  } else {
    iree_net_shm_listener_setup_drained(slot);
  }
}

#if defined(IREE_PLATFORM_WINDOWS)
static void iree_net_shm_listener_pipe_accepted(void* user_data,
                                                iree_status_t status) {
  iree_net_shm_listener_slot_t* slot = user_data;
  slot->status = status;
  if (slot->phase == IREE_NET_SHM_LISTENER_SLOT_DRAINING_SETUP) {
    // The helper's drain callback owns channel destruction after this return.
    if (iree_status_is_cancelled(slot->status)) {
      iree_status_free(slot->status);
      slot->status = iree_ok_status();
    }
    return;
  }
  iree_net_shm_listener_accept_done(slot);
}
#else
static void iree_net_shm_listener_accept_cancellation_complete(
    void* user_data) {
  iree_net_shm_listener_slot_t* slot = user_data;
  slot->flags &= ~IREE_NET_SHM_LISTENER_SLOT_FLAG_CANCELLATION_PENDING;
  if (!iree_any_bit_set(slot->flags,
                        IREE_NET_SHM_LISTENER_SLOT_FLAG_NATIVE_PENDING)) {
    iree_net_shm_listener_accept_done(slot);
  }
}

static void iree_net_shm_listener_socket_accepted(
    void* user_data, iree_async_operation_t* operation, iree_status_t status,
    iree_async_completion_flags_t flags) {
  (void)operation;
  (void)flags;
  iree_net_shm_listener_slot_t* slot = user_data;
  slot->channel.socket = slot->accept.accepted_socket;
  slot->accept.accepted_socket = NULL;
  slot->status = status;
  slot->flags &= ~IREE_NET_SHM_LISTENER_SLOT_FLAG_NATIVE_PENDING;
  if (iree_any_bit_set(slot->flags,
                       IREE_NET_SHM_LISTENER_SLOT_FLAG_CANCELLATION_PENDING)) {
    iree_async_proactor_cancel_request_target_retired(slot->listener->proactor,
                                                      &slot->cancellation);
  } else {
    iree_net_shm_listener_accept_done(slot);
  }
}
#endif

static void iree_net_shm_listener_start_slot(
    iree_net_shm_listener_slot_t* slot) {
  iree_net_shm_listener_t* listener = slot->listener;
  if (!iree_net_shm_listener_is_listening(listener)) {
    return;
  }
  iree_status_t status = iree_ok_status();
#if defined(IREE_PLATFORM_WINDOWS)
  if (slot->channel.pipe.type == IREE_ASYNC_PRIMITIVE_TYPE_NONE) {
    status = iree_async_local_stream_pipe_create(
        listener->address, 255, IREE_ASYNC_LOCAL_STREAM_PIPE_FLAG_NONE,
        &slot->channel.pipe);
  }
  if (iree_status_is_ok(status)) {
    iree_async_primitive_t name_guard = {0};
    status = iree_async_primitive_dup(slot->channel.pipe, &name_guard);
    if (iree_status_is_ok(status)) {
      iree_async_primitive_close(&listener->name_guard);
      listener->name_guard = name_guard;
      status = iree_async_local_stream_allocate(
          listener->proactor, slot->channel.pipe,
          IREE_NET_SHM_STORAGE_HANDLE_COUNT, listener->host_allocator,
          &slot->channel.stream);
    }
  }
  if (iree_status_is_ok(status)) {
    slot->phase = IREE_NET_SHM_LISTENER_SLOT_ACCEPTING;
    IREE_CHECK_OK(iree_async_local_stream_pipe_accept(
        slot->channel.stream, (iree_async_local_stream_callback_t){
                                  .fn = iree_net_shm_listener_pipe_accepted,
                                  .user_data = slot,
                              }));
  }
#else
  iree_async_operation_zero(&slot->accept.base, sizeof(slot->accept));
  iree_async_operation_initialize(&slot->accept.base,
                                  IREE_ASYNC_OPERATION_TYPE_SOCKET_ACCEPT,
                                  IREE_ASYNC_OPERATION_FLAG_NONE,
                                  iree_net_shm_listener_socket_accepted, slot);
  slot->accept.listen_socket = listener->socket;
  status =
      iree_async_proactor_submit_one(listener->proactor, &slot->accept.base);
  if (iree_status_is_ok(status)) {
    slot->phase = IREE_NET_SHM_LISTENER_SLOT_ACCEPTING;
    slot->flags = IREE_NET_SHM_LISTENER_SLOT_FLAG_NATIVE_PENDING;
  }
#endif
  if (!iree_status_is_ok(status)) {
    iree_net_shm_connection_channel_deinitialize(&slot->channel);
    iree_net_shm_listener_fail(listener, status);
  }
}

static void iree_net_shm_listener_control_complete(
    void* user_data, iree_async_operation_t* operation, iree_status_t status,
    iree_async_completion_flags_t flags) {
  (void)operation;
  (void)flags;
  iree_net_shm_listener_t* listener = user_data;
  iree_slim_mutex_lock(&listener->mutex);
  listener->control_queued = false;
  iree_slim_mutex_unlock(&listener->mutex);
  if (!iree_status_is_ok(status)) {
    iree_net_shm_listener_fail(listener, status);
  }
  for (uint32_t i = 0; i < listener->slot_count; ++i) {
    iree_net_shm_listener_slot_t* slot = &listener->slots[i];
    if (iree_net_shm_listener_is_listening(listener)) {
      if (slot->phase == IREE_NET_SHM_LISTENER_SLOT_IDLE) {
        iree_net_shm_listener_start_slot(slot);
      }
    } else {
      switch (slot->phase) {
        case IREE_NET_SHM_LISTENER_SLOT_IDLE:
          iree_net_shm_connection_channel_deinitialize(&slot->channel);
          break;
        case IREE_NET_SHM_LISTENER_SLOT_ACCEPTING:
#if defined(IREE_PLATFORM_WINDOWS)
          slot->phase = IREE_NET_SHM_LISTENER_SLOT_DRAINING_SETUP;
          IREE_CHECK_OK(iree_async_local_stream_deactivate(
              slot->channel.stream,
              (iree_async_local_stream_deactivated_callback_t){
                  .fn = iree_net_shm_listener_setup_drained,
                  .user_data = slot,
              }));
#else
          if (!iree_any_bit_set(
                  slot->flags,
                  IREE_NET_SHM_LISTENER_SLOT_FLAG_CANCELLATION_PENDING)) {
            slot->flags |= IREE_NET_SHM_LISTENER_SLOT_FLAG_CANCELLATION_PENDING;
            iree_async_cancel_request_initialize(
                (iree_async_cancel_callback_t){
                    .fn = iree_net_shm_listener_accept_cancellation_complete,
                    .user_data = slot,
                },
                &slot->cancellation);
            IREE_CHECK_OK(iree_async_proactor_request_cancel(
                listener->proactor, &slot->accept.base, &slot->cancellation));
          }
#endif
          break;
        case IREE_NET_SHM_LISTENER_SLOT_HANDSHAKING:
          iree_net_shm_handshake_cancel(&slot->handshake);
          break;
        case IREE_NET_SHM_LISTENER_SLOT_DRAINING_SETUP:
        case IREE_NET_SHM_LISTENER_SLOT_DRAINING_RESULT:
          break;
      }
    }
  }
  if (!iree_net_shm_listener_is_listening(listener)) {
    iree_status_t name_status = iree_net_shm_listener_release_name(listener);
    if (!iree_status_is_ok(name_status)) {
      listener->accept_callback.fn(listener->accept_callback.user_data,
                                   name_status, NULL);
    }
  }
  iree_slim_mutex_lock(&listener->mutex);
  --listener->pending_control_count;
  iree_slim_mutex_unlock(&listener->mutex);
  iree_net_shm_listener_finish_stop(listener);
}

static void iree_net_shm_listener_free(iree_net_listener_t* base) {
  iree_net_shm_listener_t* listener = (iree_net_shm_listener_t*)base;
  IREE_ASSERT(listener->state == IREE_NET_SHM_LISTENER_STATE_STOPPED,
              "SHM listener freed before stop completed");
  iree_allocator_t host_allocator = listener->host_allocator;
#if !defined(IREE_PLATFORM_WINDOWS)
  iree_async_socket_release(listener->socket);
#endif
  iree_async_proactor_release(listener->proactor);
  iree_slim_mutex_deinitialize(&listener->mutex);
  iree_allocator_free(host_allocator, listener);
}

static iree_status_t iree_net_shm_listener_stop(
    iree_net_listener_t* base, iree_net_listener_stopped_callback_t callback) {
  iree_net_shm_listener_t* listener = (iree_net_shm_listener_t*)base;
  iree_status_t status = iree_ok_status();
  iree_slim_mutex_lock(&listener->mutex);
  if (listener->state == IREE_NET_SHM_LISTENER_STATE_STOPPING ||
      listener->state == IREE_NET_SHM_LISTENER_STATE_STOPPED) {
    status = iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                              "SHM listener stop already requested");
  } else {
    listener->state = IREE_NET_SHM_LISTENER_STATE_STOPPING;
    listener->stopped_callback = callback;
    iree_net_shm_listener_kick_locked(listener);
  }
  iree_slim_mutex_unlock(&listener->mutex);
  return status;
}

static iree_status_t iree_net_shm_listener_query_bound_address(
    iree_net_listener_t* base, iree_host_size_t buffer_capacity, char* buffer,
    iree_string_view_t* out_address) {
  iree_net_shm_listener_t* listener = (iree_net_shm_listener_t*)base;
  if (!out_address) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "SHM bound address output is required");
  }
  *out_address = iree_string_view_empty();
  if (!buffer || buffer_capacity < listener->address.size) {
    return iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                            "SHM bound address requires %" PRIhsz " bytes",
                            listener->address.size);
  }
  memcpy(buffer, listener->address.data, listener->address.size);
  *out_address = iree_make_string_view(buffer, listener->address.size);
  return iree_ok_status();
}

static const iree_net_listener_vtable_t iree_net_shm_listener_vtable = {
    .free = iree_net_shm_listener_free,
    .stop = iree_net_shm_listener_stop,
    .query_bound_address = iree_net_shm_listener_query_bound_address,
};

iree_status_t iree_net_shm_listener_allocate(
    iree_string_view_t address, iree_async_proactor_t* proactor,
    const iree_net_shm_region_layout_t* layout,
    const iree_net_shm_carrier_options_t* carrier_options,
    uint32_t max_pending_connections,
    iree_net_listener_accept_callback_t callback,
    iree_allocator_t host_allocator, iree_net_listener_t** out_listener) {
  *out_listener = NULL;
  if (!proactor || !address.data || iree_string_view_is_empty(address)) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "SHM listener requires a proactor and address");
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
  iree_host_size_t slots_offset = 0;
  iree_host_size_t address_offset = 0;
  IREE_RETURN_IF_ERROR(IREE_STRUCT_LAYOUT(
      sizeof(iree_net_shm_listener_t), &allocation_size,
      IREE_STRUCT_FIELD(max_pending_connections, iree_net_shm_listener_slot_t,
                        &slots_offset),
      IREE_STRUCT_FIELD(address.size, char, &address_offset),
      IREE_STRUCT_FIELD(1, char, NULL)));
  iree_net_shm_listener_t* listener = NULL;
  IREE_RETURN_IF_ERROR(iree_allocator_malloc(host_allocator, allocation_size,
                                             (void**)&listener));
  memset(listener, 0, allocation_size);
  listener->base.vtable = &iree_net_shm_listener_vtable;
  listener->host_allocator = host_allocator;
  listener->proactor = proactor;
  iree_async_proactor_retain(proactor);
  iree_slim_mutex_initialize(&listener->mutex);
  listener->layout = *layout;
  listener->carrier_options = *carrier_options;
  listener->accept_callback = callback;
  listener->slot_count = max_pending_connections;
  listener->slots =
      (iree_net_shm_listener_slot_t*)((uint8_t*)listener + slots_offset);
  char* address_storage = (char*)listener + address_offset;
  memcpy(address_storage, address.data, address.size);
  listener->address = iree_make_string_view(address_storage, address.size);
  listener->control.base.completion_fn = iree_net_shm_listener_control_complete;
  for (uint32_t i = 0; i < max_pending_connections; ++i) {
    listener->slots[i].listener = listener;
  }
  iree_status_t status = iree_ok_status();
#if defined(IREE_PLATFORM_WINDOWS)
  for (uint32_t i = 0; i < max_pending_connections && iree_status_is_ok(status);
       ++i) {
    status = iree_async_local_stream_pipe_create(
        address, 255,
        i == 0 ? IREE_ASYNC_LOCAL_STREAM_PIPE_FLAG_FIRST_INSTANCE
               : IREE_ASYNC_LOCAL_STREAM_PIPE_FLAG_NONE,
        &listener->slots[i].channel.pipe);
  }
  if (iree_status_is_ok(status)) {
    status = iree_async_primitive_dup(listener->slots[0].channel.pipe,
                                      &listener->name_guard);
  }
#else
  status = iree_async_socket_create(
      proactor, IREE_ASYNC_SOCKET_TYPE_UNIX_STREAM,
      IREE_ASYNC_SOCKET_OPTION_NONE, &listener->socket);
  if (iree_status_is_ok(status)) {
    status = iree_async_socket_bind(listener->socket, &native_address);
  }
  if (iree_status_is_ok(status) && address.data[0] != '@') {
    struct stat metadata;
    if (lstat(address_storage, &metadata) != 0) {
      status = iree_make_status(iree_status_code_from_errno(errno),
                                "inspecting bound SHM listener path");
    } else {
      listener->path.owned = true;
      listener->path.device = metadata.st_dev;
      listener->path.inode = metadata.st_ino;
    }
  }
  if (iree_status_is_ok(status)) {
    status =
        iree_async_socket_listen(listener->socket, max_pending_connections);
  }
#endif
  if (iree_status_is_ok(status)) {
    iree_slim_mutex_lock(&listener->mutex);
    iree_net_shm_listener_kick_locked(listener);
    iree_slim_mutex_unlock(&listener->mutex);
    *out_listener = &listener->base;
  } else {
    // No owner work has been admitted: only native setup handles exist here.
    for (uint32_t i = 0; i < max_pending_connections; ++i) {
      iree_net_shm_connection_channel_deinitialize(&listener->slots[i].channel);
    }
    status =
        iree_status_join(status, iree_net_shm_listener_release_name(listener));
    listener->state = IREE_NET_SHM_LISTENER_STATE_STOPPED;
    iree_net_shm_listener_free(&listener->base);
  }
  return status;
}
