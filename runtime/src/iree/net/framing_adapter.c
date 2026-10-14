// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/net/framing_adapter.h"

#include <string.h>

#include "iree/base/threading/mutex.h"
#include "iree/net/buffer_lease.h"

struct iree_net_framing_adapter_t {
  // Serializes callback bundle replacement and snapshots.
  iree_slim_mutex_t callback_mutex;

  // Owned carrier - released when adapter is freed.
  iree_net_carrier_t* carrier;

  // Message handlers protected by |callback_mutex|.
  iree_net_message_endpoint_callbacks_t callbacks;

  // First terminal receive failure, retained for destination retirement.
  iree_status_t terminal_status;

  // Coordinates endpoint and connection deactivation requests.
  iree_net_endpoint_lifecycle_t lifecycle;

  // Host allocator owning the adapter and reassembled message leases.
  iree_allocator_t host_allocator;

  // Embedded frame accumulator with FAM for internal buffer - must be last.
  iree_net_frame_accumulator_t accumulator;
};

static iree_net_message_endpoint_callbacks_t
iree_net_framing_adapter_snapshot_callbacks(
    iree_net_framing_adapter_t* adapter) {
  iree_slim_mutex_lock(&adapter->callback_mutex);
  iree_net_message_endpoint_callbacks_t callbacks = adapter->callbacks;
  iree_slim_mutex_unlock(&adapter->callback_mutex);
  return callbacks;
}

// Called by frame_accumulator when a complete frame is ready.
// Bridges a copy-path frame to independently owned host storage.
static iree_status_t iree_net_framing_adapter_on_frame_complete(
    void* user_data, iree_const_byte_span_t frame,
    iree_async_buffer_lease_t* lease) {
  iree_net_framing_adapter_t* adapter = (iree_net_framing_adapter_t*)user_data;
  const iree_net_message_endpoint_callbacks_t callbacks =
      iree_net_framing_adapter_snapshot_callbacks(adapter);

  if (lease) {
    return callbacks.on_message(callbacks.user_data, frame, lease);
  }

  // Borrowed path: the carrier needs its storage back or the frame precedes
  // later frames in a shared receive buffer. Give it independent ownership
  // without retaining a registered buffer needed for transport progress.
  iree_async_buffer_lease_t bridged_lease;
  IREE_RETURN_IF_ERROR(iree_net_buffer_lease_allocate(
      frame.data_length, adapter->host_allocator, &bridged_lease));

  uint8_t* dest = iree_async_span_ptr(bridged_lease.span);
  memcpy(dest, frame.data, frame.data_length);
  iree_const_byte_span_t bridged_frame =
      iree_make_const_byte_span(dest, frame.data_length);

  iree_status_t status =
      callbacks.on_message(callbacks.user_data, bridged_frame, &bridged_lease);

  // Release unless the handler moved the lease and cleared this value.
  iree_async_buffer_lease_release(&bridged_lease);
  return status;
}

static iree_status_t iree_net_framing_adapter_on_recv(
    void* user_data, iree_async_span_t data, iree_async_buffer_lease_t* lease) {
  iree_net_framing_adapter_t* adapter = (iree_net_framing_adapter_t*)user_data;
  if (data.length == 0) {
    const iree_host_size_t buffered_bytes =
        iree_net_frame_accumulator_buffered_bytes(&adapter->accumulator);
    if (buffered_bytes > 0) {
      return iree_make_status(IREE_STATUS_DATA_LOSS,
                              "carrier peer closed with %" PRIhsz
                              " bytes of a partial frame",
                              buffered_bytes);
    }
    return iree_make_status(IREE_STATUS_UNAVAILABLE,
                            "carrier peer closed the receive stream");
  }
  if (lease) {
    return iree_net_frame_accumulator_push_lease(&adapter->accumulator, data,
                                                 lease);
  }
  return iree_net_frame_accumulator_push_span(&adapter->accumulator, data);
}

static void iree_net_framing_adapter_on_carrier_error(void* user_data,
                                                      iree_status_t status) {
  iree_net_framing_adapter_t* adapter = (iree_net_framing_adapter_t*)user_data;
  iree_slim_mutex_lock(&adapter->callback_mutex);
  adapter->terminal_status =
      iree_status_join(adapter->terminal_status, iree_status_clone(status));
  iree_slim_mutex_unlock(&adapter->callback_mutex);
  const iree_net_message_endpoint_callbacks_t callbacks =
      iree_net_framing_adapter_snapshot_callbacks(adapter);
  callbacks.on_error(callbacks.user_data, status);
}

static void iree_net_framing_adapter_on_carrier_deactivated(void* user_data) {
  iree_net_framing_adapter_t* adapter = (iree_net_framing_adapter_t*)user_data;
  if (adapter->accumulator.receive_operation) {
    iree_slim_mutex_lock(&adapter->callback_mutex);
    iree_status_t status = iree_status_clone(adapter->terminal_status);
    iree_slim_mutex_unlock(&adapter->callback_mutex);
    if (iree_status_is_ok(status)) {
      status = iree_status_from_code(IREE_STATUS_CANCELLED);
    }
    iree_net_frame_accumulator_abort(&adapter->accumulator, status);
  } else {
    iree_net_frame_accumulator_reset(&adapter->accumulator);
  }
  iree_net_endpoint_lifecycle_complete_deactivation(&adapter->lifecycle);
}

static void iree_net_framing_adapter_set_callbacks(
    void* self, iree_net_message_endpoint_callbacks_t callbacks) {
  iree_net_framing_adapter_t* adapter = (iree_net_framing_adapter_t*)self;
  iree_slim_mutex_lock(&adapter->callback_mutex);
  adapter->callbacks = callbacks;
  iree_slim_mutex_unlock(&adapter->callback_mutex);
}

static iree_status_t iree_net_framing_adapter_activate(void* self) {
  iree_net_framing_adapter_t* adapter = (iree_net_framing_adapter_t*)self;
  const iree_net_message_endpoint_callbacks_t callbacks =
      iree_net_framing_adapter_snapshot_callbacks(adapter);
  if (!callbacks.on_message || !callbacks.on_error) {
    return iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                            "message and error callbacks are required");
  }
  IREE_RETURN_IF_ERROR(
      iree_net_endpoint_lifecycle_activate(&adapter->lifecycle));
  iree_net_carrier_handlers_t handlers = {
      .on_receive = iree_net_framing_adapter_on_recv,
      .on_error = iree_net_framing_adapter_on_carrier_error,
      .user_data = adapter,
  };

  iree_status_t status =
      iree_net_carrier_set_handlers(adapter->carrier, handlers);
  if (iree_status_is_ok(status)) {
    status = iree_net_carrier_activate(adapter->carrier);
  }
  if (!iree_status_is_ok(status)) {
    iree_net_endpoint_lifecycle_rollback_activation(&adapter->lifecycle);
  }
  return status;
}

static iree_status_t iree_net_framing_adapter_deactivate(
    void* self, iree_net_message_endpoint_deactivate_fn_t callback,
    void* user_data) {
  iree_net_framing_adapter_t* adapter = (iree_net_framing_adapter_t*)self;
  iree_net_endpoint_lifecycle_actions_t actions =
      IREE_NET_ENDPOINT_LIFECYCLE_ACTION_NONE;
  IREE_RETURN_IF_ERROR(iree_net_endpoint_lifecycle_request_deactivation(
      &adapter->lifecycle, callback, user_data, &actions));
  if (iree_any_bit_set(actions,
                       IREE_NET_ENDPOINT_LIFECYCLE_ACTION_BEGIN_DEACTIVATION)) {
    iree_net_carrier_deactivate(adapter->carrier,
                                iree_net_framing_adapter_on_carrier_deactivated,
                                adapter);
  }
  return iree_ok_status();
}

static iree_status_t iree_net_framing_adapter_send(
    void* self, const iree_net_message_endpoint_send_params_t* params) {
  iree_net_framing_adapter_t* adapter = (iree_net_framing_adapter_t*)self;
  iree_net_send_params_t carrier_params = {
      .generated_prefix = params->generated_prefix,
      .data = params->data,
      .completion_callback = params->completion_callback,
  };
  return iree_net_carrier_send(adapter->carrier, &carrier_params);
}

static iree_net_carrier_send_budget_t
iree_net_framing_adapter_query_send_budget(void* self) {
  iree_net_framing_adapter_t* adapter = (iree_net_framing_adapter_t*)self;
  return iree_net_carrier_query_send_budget(adapter->carrier);
}

iree_status_t iree_net_framing_adapter_allocate(
    iree_net_carrier_t* carrier, iree_net_frame_length_callback_t frame_length,
    iree_host_size_t max_frame_size,
    iree_net_endpoint_deactivation_barrier_t* connection_barrier,
    iree_allocator_t host_allocator, iree_net_framing_adapter_t** out_adapter) {
  IREE_ASSERT_ARGUMENT(out_adapter);
  *out_adapter = NULL;

  if (!carrier) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "carrier is required");
  }
  if (!frame_length.fn || frame_length.header_size == 0) {
    return iree_make_status(
        IREE_STATUS_INVALID_ARGUMENT,
        "frame length callback and header size are required");
  }
  if (max_frame_size == 0) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "max_frame_size must be > 0");
  }
  if (frame_length.header_size > max_frame_size) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "maximum header size exceeds maximum frame size");
  }
  if (iree_net_carrier_state(carrier) != IREE_NET_CARRIER_STATE_CREATED) {
    return iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                            "carrier must not be activated before wrapping");
  }
  IREE_TRACE_ZONE_BEGIN(z0);

  iree_host_size_t accumulator_storage_size = 0;
  iree_status_t status = iree_net_frame_accumulator_calculate_storage_size(
      frame_length.header_size, &accumulator_storage_size);
  iree_host_size_t total_size = 0;
  if (iree_status_is_ok(status) &&
      !iree_host_size_checked_add(
          offsetof(iree_net_framing_adapter_t, accumulator),
          accumulator_storage_size, &total_size)) {
    status = iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                              "framing adapter allocation size overflow");
  }

  iree_net_framing_adapter_t* adapter = NULL;
  if (iree_status_is_ok(status)) {
    status =
        iree_allocator_malloc(host_allocator, total_size, (void**)&adapter);
  }
  if (iree_status_is_ok(status)) {
    memset(adapter, 0, total_size);
    adapter->host_allocator = host_allocator;
    iree_slim_mutex_initialize(&adapter->callback_mutex);
    iree_net_endpoint_lifecycle_initialize(connection_barrier,
                                           &adapter->lifecycle);
  }

  iree_net_frame_complete_callback_t on_frame_complete = {
      .fn = iree_net_framing_adapter_on_frame_complete,
      .user_data = adapter,
  };
  if (iree_status_is_ok(status)) {
    status = iree_net_frame_accumulator_initialize(
        &adapter->accumulator, max_frame_size, frame_length, on_frame_complete,
        host_allocator);
  }

  if (iree_status_is_ok(status)) {
    // Carrier ownership transfers only after the adapter is fully initialized.
    // On failure the caller retains its reference.
    adapter->carrier = carrier;
    *out_adapter = adapter;
  } else if (adapter) {
    iree_net_framing_adapter_free(adapter);
  }
  IREE_TRACE_ZONE_END(z0);
  return status;
}

void iree_net_framing_adapter_free(iree_net_framing_adapter_t* adapter) {
  if (!adapter) {
    return;
  }
  IREE_TRACE_ZONE_BEGIN(z0);
  iree_allocator_t allocator = adapter->host_allocator;
  iree_net_endpoint_lifecycle_deinitialize(&adapter->lifecycle);
  iree_net_frame_accumulator_deinitialize(&adapter->accumulator);
  iree_status_free(adapter->terminal_status);
  iree_net_carrier_release(adapter->carrier);
  iree_slim_mutex_deinitialize(&adapter->callback_mutex);
  iree_allocator_free(allocator, adapter);
  IREE_TRACE_ZONE_END(z0);
}

void iree_net_framing_adapter_set_receive_router(
    iree_net_framing_adapter_t* adapter,
    iree_net_frame_receive_router_t router) {
  iree_net_frame_accumulator_set_receive_router(&adapter->accumulator, router);
}

iree_async_span_t iree_net_framing_adapter_query_receive_buffer(
    iree_net_framing_adapter_t* adapter) {
  return iree_net_frame_accumulator_query_receive_buffer(&adapter->accumulator);
}

void iree_net_framing_adapter_join_deactivation(
    iree_net_framing_adapter_t* adapter) {
  IREE_ASSERT_ARGUMENT(adapter);
  iree_net_endpoint_lifecycle_actions_t actions =
      iree_net_endpoint_lifecycle_join_deactivation(&adapter->lifecycle);
  if (iree_any_bit_set(actions,
                       IREE_NET_ENDPOINT_LIFECYCLE_ACTION_BEGIN_DEACTIVATION)) {
    iree_net_carrier_deactivate(adapter->carrier,
                                iree_net_framing_adapter_on_carrier_deactivated,
                                adapter);
  }
}

iree_net_message_endpoint_t iree_net_framing_adapter_as_endpoint(
    iree_net_framing_adapter_t* adapter) {
  IREE_ASSERT_ARGUMENT(adapter);
  static const iree_net_message_endpoint_vtable_t vtable = {
      .set_callbacks = iree_net_framing_adapter_set_callbacks,
      .activate = iree_net_framing_adapter_activate,
      .deactivate = iree_net_framing_adapter_deactivate,
      .send = iree_net_framing_adapter_send,
      .query_send_budget = iree_net_framing_adapter_query_send_budget,
  };
  iree_net_message_endpoint_t endpoint = {
      .self = adapter,
      .vtable = &vtable,
  };
  return endpoint;
}
