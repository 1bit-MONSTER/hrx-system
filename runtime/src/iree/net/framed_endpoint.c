// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/net/framed_endpoint.h"

#include <string.h>

#include "iree/async/operations/scheduling.h"
#include "iree/base/threading/mutex.h"
#include "iree/net/framing_adapter.h"
#include "iree/net/message_receive_queue.h"

// "IRN1" in little-endian byte order.
#define IREE_NET_FRAMED_ENDPOINT_FRAME_MAGIC UINT32_C(0x314E5249)

#define IREE_NET_FRAMED_ENDPOINT_FRAME_HEADER_SIZE 8u

#define IREE_NET_FRAMED_ENDPOINT_SEND_STATE_NONE UINT32_MAX

typedef enum iree_net_framed_endpoint_state_e {
  IREE_NET_FRAMED_ENDPOINT_STATE_CREATED = 0,
  IREE_NET_FRAMED_ENDPOINT_STATE_ACTIVE = 1,
  IREE_NET_FRAMED_ENDPOINT_STATE_DRAINING = 2,
  IREE_NET_FRAMED_ENDPOINT_STATE_DEACTIVATED = 3,
} iree_net_framed_endpoint_state_t;

typedef enum iree_net_framed_endpoint_send_state_phase_e {
  IREE_NET_FRAMED_ENDPOINT_SEND_STATE_PHASE_FREE = 0,
  IREE_NET_FRAMED_ENDPOINT_SEND_STATE_PHASE_IN_FLIGHT = 1,
} iree_net_framed_endpoint_send_state_phase_t;

typedef struct iree_net_framed_endpoint_send_state_t {
  // Endpoint owning this state record.
  struct iree_net_framed_endpoint_t* endpoint;

  // Index of this record in its endpoint's trailing state array.
  uint32_t index;

  // Next free record index while this state is available.
  uint32_t next_free;

  // Current ownership phase for this state record.
  iree_net_framed_endpoint_send_state_phase_t phase;

  // Payload bytes represented by this framed send.
  iree_host_size_t payload_length;

  // User completion invoked after this state is returned to the pool.
  iree_net_send_completion_callback_t completion_callback;

  // Dispatches a locally terminated send on the endpoint proactor.
  iree_async_nop_operation_t local_completion_operation;

  // Owned status transferred through |local_completion_operation|.
  iree_status_t local_completion_status;
} iree_net_framed_endpoint_send_state_t;

struct iree_net_framed_endpoint_t {
  // Serializes lifecycle transitions and send-state allocation.
  iree_slim_mutex_t mutex;

  // Proactor owning endpoint callbacks. Retained.
  iree_async_proactor_t* proactor;

  // Current endpoint lifecycle state.
  iree_net_framed_endpoint_state_t state;

  // Message callbacks installed by the endpoint consumer.
  iree_net_message_endpoint_callbacks_t callbacks;

  // Coordinates accepted sends with endpoint and connection drain.
  iree_net_endpoint_lifecycle_t lifecycle;

  // Posted body destinations sharing this endpoint's native drain.
  iree_net_message_receive_queue_t receive_queue;

  // Owned wire-frame adapter and carrier stack.
  iree_net_framing_adapter_t* framing_adapter;

  // Borrowed endpoint view into |framing_adapter|.
  iree_net_message_endpoint_t wire_endpoint;

  // Host allocator owning this endpoint and its framing adapter.
  iree_allocator_t host_allocator;

  // Number of records in the trailing send-state array.
  uint32_t send_state_count;

  // Number of records currently available for admission.
  uint32_t free_send_state_count;

  // Head index of the intrusive send-state free list.
  uint32_t free_send_state_head;

  // Fixed send completion records.
  iree_net_framed_endpoint_send_state_t send_states[];
};

typedef struct iree_net_framed_endpoint_frame_prefix_t {
  // Total frame extent encoded into the wire header.
  uint32_t frame_length;

  // Message prefix generated after the wire header.
  iree_net_send_prefix_t message_prefix;
} iree_net_framed_endpoint_frame_prefix_t;

static void iree_net_framed_endpoint_encode_frame_header(
    uint8_t* header, uint32_t frame_length) {
  iree_unaligned_store_le_u32(header + 0, IREE_NET_FRAMED_ENDPOINT_FRAME_MAGIC);
  iree_unaligned_store_le_u32(header + 4, frame_length);
}

static iree_status_t iree_net_framed_endpoint_write_frame_prefix(
    void* user_data, iree_byte_span_t target) {
  iree_net_framed_endpoint_frame_prefix_t* prefix =
      (iree_net_framed_endpoint_frame_prefix_t*)user_data;
  iree_net_framed_endpoint_encode_frame_header(target.data,
                                               prefix->frame_length);
  if (prefix->message_prefix.length == 0) {
    return iree_ok_status();
  }
  return prefix->message_prefix.write(
      prefix->message_prefix.user_data,
      iree_make_byte_span(
          target.data + IREE_NET_FRAMED_ENDPOINT_FRAME_HEADER_SIZE,
          prefix->message_prefix.length));
}

static iree_status_t iree_net_framed_endpoint_calculate_frame_length(
    iree_host_size_t payload_length, uint32_t* out_frame_length) {
  if (payload_length == 0) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "framed messages must be nonempty");
  }
  if (payload_length > (iree_host_size_t)UINT32_MAX -
                           IREE_NET_FRAMED_ENDPOINT_FRAME_HEADER_SIZE) {
    return iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                            "framed message length %" PRIhsz
                            " exceeds the 32-bit wire extent",
                            payload_length);
  }
  *out_frame_length =
      (uint32_t)(payload_length + IREE_NET_FRAMED_ENDPOINT_FRAME_HEADER_SIZE);
  return iree_ok_status();
}

static iree_status_t iree_net_framed_endpoint_resolve_frame_length(
    void* user_data, iree_const_byte_span_t available,
    iree_host_size_t* out_frame_size) {
  (void)user_data;
  *out_frame_size = 0;
  if (available.data_length < IREE_NET_FRAMED_ENDPOINT_FRAME_HEADER_SIZE) {
    return iree_ok_status();
  }
  const uint32_t magic = iree_unaligned_load_le_u32(available.data + 0);
  if (magic != IREE_NET_FRAMED_ENDPOINT_FRAME_MAGIC) {
    return iree_make_status(IREE_STATUS_DATA_LOSS, "invalid frame magic 0x%08X",
                            magic);
  }
  const uint32_t frame_length = iree_unaligned_load_le_u32(available.data + 4);
  if (frame_length <= IREE_NET_FRAMED_ENDPOINT_FRAME_HEADER_SIZE) {
    return iree_make_status(IREE_STATUS_DATA_LOSS, "invalid frame length %u",
                            frame_length);
  }
  *out_frame_size = frame_length;
  return iree_ok_status();
}

static iree_net_framed_endpoint_send_state_t*
iree_net_framed_endpoint_acquire_send_state_locked(
    iree_net_framed_endpoint_t* endpoint) {
  if (endpoint->free_send_state_head ==
      IREE_NET_FRAMED_ENDPOINT_SEND_STATE_NONE) {
    return NULL;
  }
  iree_net_framed_endpoint_send_state_t* send_state =
      &endpoint->send_states[endpoint->free_send_state_head];
  endpoint->free_send_state_head = send_state->next_free;
  --endpoint->free_send_state_count;
  send_state->next_free = IREE_NET_FRAMED_ENDPOINT_SEND_STATE_NONE;
  send_state->phase = IREE_NET_FRAMED_ENDPOINT_SEND_STATE_PHASE_IN_FLIGHT;
  return send_state;
}

static void iree_net_framed_endpoint_release_send_state_locked(
    iree_net_framed_endpoint_t* endpoint,
    iree_net_framed_endpoint_send_state_t* send_state) {
  IREE_ASSERT(iree_status_is_ok(send_state->local_completion_status));
  send_state->phase = IREE_NET_FRAMED_ENDPOINT_SEND_STATE_PHASE_FREE;
  send_state->payload_length = 0;
  send_state->completion_callback = (iree_net_send_completion_callback_t){0};
  send_state->next_free = endpoint->free_send_state_head;
  endpoint->free_send_state_head = send_state->index;
  ++endpoint->free_send_state_count;
}

static void iree_net_framed_endpoint_send_complete(
    void* user_data, iree_status_t status,
    iree_host_size_t wire_bytes_transferred) {
  iree_net_framed_endpoint_send_state_t* send_state =
      (iree_net_framed_endpoint_send_state_t*)user_data;
  iree_net_framed_endpoint_t* endpoint = send_state->endpoint;

  iree_slim_mutex_lock(&endpoint->mutex);
  IREE_ASSERT(
      send_state->phase == IREE_NET_FRAMED_ENDPOINT_SEND_STATE_PHASE_IN_FLIGHT,
      "framed send completed from phase %d", (int)send_state->phase);
  const iree_host_size_t payload_length = send_state->payload_length;
  iree_net_send_completion_callback_t completion_callback =
      send_state->completion_callback;
  iree_net_framed_endpoint_release_send_state_locked(endpoint, send_state);
  iree_slim_mutex_unlock(&endpoint->mutex);

  IREE_ASSERT(
      !iree_status_is_ok(status) ||
          wire_bytes_transferred ==
              payload_length + IREE_NET_FRAMED_ENDPOINT_FRAME_HEADER_SIZE,
      "successful framed send completed %" PRIhsz " of %" PRIhsz " wire bytes",
      wire_bytes_transferred,
      payload_length + IREE_NET_FRAMED_ENDPOINT_FRAME_HEADER_SIZE);
  iree_host_size_t payload_bytes_transferred = 0;
  if (wire_bytes_transferred > IREE_NET_FRAMED_ENDPOINT_FRAME_HEADER_SIZE) {
    payload_bytes_transferred = iree_min(
        payload_length,
        wire_bytes_transferred - IREE_NET_FRAMED_ENDPOINT_FRAME_HEADER_SIZE);
  }
  completion_callback.fn(completion_callback.user_data, status,
                         payload_bytes_transferred);
  iree_net_endpoint_lifecycle_end_operation(&endpoint->lifecycle);
}

static void iree_net_framed_endpoint_local_send_complete(
    void* user_data, iree_async_operation_t* operation, iree_status_t status,
    iree_async_completion_flags_t flags) {
  (void)operation;
  IREE_ASSERT(!iree_any_bit_set(flags, IREE_ASYNC_COMPLETION_FLAG_MORE));
  iree_net_framed_endpoint_send_state_t* send_state =
      (iree_net_framed_endpoint_send_state_t*)user_data;
  iree_status_t completion_status = send_state->local_completion_status;
  send_state->local_completion_status = iree_ok_status();
  completion_status = iree_status_join(completion_status, status);
  iree_net_framed_endpoint_send_complete(send_state, completion_status,
                                         /*wire_bytes_transferred=*/0);
}

static iree_status_t iree_net_framed_endpoint_on_wire_message(
    void* user_data, iree_const_byte_span_t frame,
    iree_async_buffer_lease_t* lease) {
  iree_net_framed_endpoint_t* endpoint = (iree_net_framed_endpoint_t*)user_data;
  IREE_ASSERT(frame.data_length > IREE_NET_FRAMED_ENDPOINT_FRAME_HEADER_SIZE,
              "validated frame must contain a payload");
  iree_const_byte_span_t payload = iree_make_const_byte_span(
      frame.data + IREE_NET_FRAMED_ENDPOINT_FRAME_HEADER_SIZE,
      frame.data_length - IREE_NET_FRAMED_ENDPOINT_FRAME_HEADER_SIZE);
  iree_slim_mutex_lock(&endpoint->mutex);
  const bool admitted =
      endpoint->state == IREE_NET_FRAMED_ENDPOINT_STATE_ACTIVE &&
      iree_net_endpoint_lifecycle_try_begin_operation(&endpoint->lifecycle);
  const iree_net_message_endpoint_callbacks_t callbacks = endpoint->callbacks;
  iree_slim_mutex_unlock(&endpoint->mutex);
  if (!admitted) {
    return iree_ok_status();
  }
  iree_status_t status =
      callbacks.on_message(callbacks.user_data, payload, lease);
  iree_net_endpoint_lifecycle_end_operation(&endpoint->lifecycle);
  return status;
}

static iree_net_message_receive_queue_t* iree_net_framed_endpoint_route_receive(
    void* user_data, iree_const_byte_span_t header) {
  (void)header;
  iree_net_framed_endpoint_t* endpoint = user_data;
  return &endpoint->receive_queue;
}

static void iree_net_framed_endpoint_on_wire_error(void* user_data,
                                                   iree_status_t status) {
  iree_net_framed_endpoint_t* endpoint = (iree_net_framed_endpoint_t*)user_data;
  iree_slim_mutex_lock(&endpoint->mutex);
  const iree_net_message_endpoint_callbacks_t callbacks = endpoint->callbacks;
  iree_slim_mutex_unlock(&endpoint->mutex);
  iree_net_message_receive_queue_close(&endpoint->receive_queue,
                                       iree_status_clone(status));
  callbacks.on_error(callbacks.user_data, status);
}

static void iree_net_framed_endpoint_set_callbacks(
    void* self, iree_net_message_endpoint_callbacks_t callbacks) {
  iree_net_framed_endpoint_t* endpoint = (iree_net_framed_endpoint_t*)self;
  iree_slim_mutex_lock(&endpoint->mutex);
  endpoint->callbacks = callbacks;
  iree_slim_mutex_unlock(&endpoint->mutex);
}

static iree_status_t iree_net_framed_endpoint_activate(void* self) {
  iree_net_framed_endpoint_t* endpoint = (iree_net_framed_endpoint_t*)self;
  iree_slim_mutex_lock(&endpoint->mutex);
  iree_status_t status = iree_ok_status();
  if (endpoint->state != IREE_NET_FRAMED_ENDPOINT_STATE_CREATED) {
    status = iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                              "framed endpoint cannot activate from state %d",
                              (int)endpoint->state);
  } else if (!endpoint->callbacks.on_message || !endpoint->callbacks.on_error) {
    status = iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                              "message and error callbacks are required");
  } else {
    status = iree_net_endpoint_lifecycle_activate(&endpoint->lifecycle);
    if (iree_status_is_ok(status)) {
      endpoint->state = IREE_NET_FRAMED_ENDPOINT_STATE_ACTIVE;
    }
  }
  iree_slim_mutex_unlock(&endpoint->mutex);
  if (!iree_status_is_ok(status)) {
    return status;
  }

  status = iree_net_message_endpoint_activate(endpoint->wire_endpoint);
  if (!iree_status_is_ok(status)) {
    iree_slim_mutex_lock(&endpoint->mutex);
    endpoint->state = IREE_NET_FRAMED_ENDPOINT_STATE_CREATED;
    iree_net_endpoint_lifecycle_rollback_activation(&endpoint->lifecycle);
    iree_slim_mutex_unlock(&endpoint->mutex);
  }
  return status;
}

static void iree_net_framed_endpoint_on_wire_deactivated(void* user_data) {
  iree_net_framed_endpoint_t* endpoint = (iree_net_framed_endpoint_t*)user_data;
  iree_slim_mutex_lock(&endpoint->mutex);
  IREE_ASSERT(endpoint->state == IREE_NET_FRAMED_ENDPOINT_STATE_DRAINING,
              "framed endpoint deactivated from state %d",
              (int)endpoint->state);
  endpoint->state = IREE_NET_FRAMED_ENDPOINT_STATE_DEACTIVATED;
  iree_slim_mutex_unlock(&endpoint->mutex);
  iree_net_endpoint_lifecycle_complete_deactivation(&endpoint->lifecycle);
}

static iree_status_t iree_net_framed_endpoint_deactivate(
    void* self, iree_net_message_endpoint_deactivate_fn_t callback,
    void* user_data) {
  iree_net_framed_endpoint_t* endpoint = (iree_net_framed_endpoint_t*)self;
  iree_slim_mutex_lock(&endpoint->mutex);
  iree_status_t status = iree_ok_status();
  if (endpoint->state != IREE_NET_FRAMED_ENDPOINT_STATE_ACTIVE) {
    status = iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                              "framed endpoint is not active (state=%d)",
                              (int)endpoint->state);
  } else {
    iree_net_endpoint_lifecycle_actions_t actions =
        IREE_NET_ENDPOINT_LIFECYCLE_ACTION_NONE;
    status = iree_net_endpoint_lifecycle_request_deactivation(
        &endpoint->lifecycle, callback, user_data, &actions);
    IREE_ASSERT(
        !iree_status_is_ok(status) ||
            iree_any_bit_set(
                actions, IREE_NET_ENDPOINT_LIFECYCLE_ACTION_BEGIN_DEACTIVATION),
        "accepted framed deactivation did not begin owner drain");
    if (iree_status_is_ok(status)) {
      endpoint->state = IREE_NET_FRAMED_ENDPOINT_STATE_DRAINING;
    }
  }
  iree_slim_mutex_unlock(&endpoint->mutex);
  if (!iree_status_is_ok(status)) {
    return status;
  }

  iree_net_message_receive_queue_close(&endpoint->receive_queue,
                                       iree_ok_status());
  status = iree_net_message_endpoint_deactivate(
      endpoint->wire_endpoint, iree_net_framed_endpoint_on_wire_deactivated,
      endpoint);
  IREE_ASSERT(iree_status_is_ok(status),
              "mirrored wire endpoint rejected valid deactivation");
  return status;
}

static iree_status_t iree_net_framed_endpoint_receive(
    void* self, iree_net_message_receive_operation_t* operation,
    const iree_net_message_endpoint_receive_params_t* params) {
  iree_net_framed_endpoint_t* endpoint = self;
  if (params->data.data_length >
      UINT32_MAX - IREE_NET_FRAMED_ENDPOINT_FRAME_HEADER_SIZE -
          params->prefix.data_length) {
    return iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                            "posted message exceeds framed endpoint limit");
  }
  return iree_net_message_receive_queue_submit(&endpoint->receive_queue,
                                               operation, params);
}

static iree_status_t iree_net_framed_endpoint_calculate_payload_length(
    const iree_net_message_endpoint_send_params_t* params,
    iree_host_size_t* out_payload_length) {
  iree_host_size_t payload_length = params->generated_prefix.length;
  for (iree_host_size_t i = 0; i < params->data.count; ++i) {
    if (!iree_host_size_checked_add(
            payload_length, params->data.values[i].length, &payload_length)) {
      return iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                              "framed message length overflow");
    }
  }
  *out_payload_length = payload_length;
  return iree_ok_status();
}

static iree_status_t iree_net_framed_endpoint_acquire_send_state(
    iree_net_framed_endpoint_t* endpoint,
    iree_net_framed_endpoint_send_state_t** out_send_state) {
  *out_send_state = NULL;
  if (!iree_net_endpoint_lifecycle_try_begin_operation(&endpoint->lifecycle)) {
    return iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                            "framed endpoint is not active");
  }

  iree_status_t status = iree_ok_status();
  iree_slim_mutex_lock(&endpoint->mutex);
  if (endpoint->state != IREE_NET_FRAMED_ENDPOINT_STATE_ACTIVE) {
    status = iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                              "framed endpoint is not active");
  } else {
    *out_send_state =
        iree_net_framed_endpoint_acquire_send_state_locked(endpoint);
    if (!*out_send_state) {
      status = iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                                "framed send operation slots are exhausted");
    }
  }
  iree_slim_mutex_unlock(&endpoint->mutex);
  if (!iree_status_is_ok(status)) {
    iree_net_endpoint_lifecycle_end_operation(&endpoint->lifecycle);
  }
  return status;
}

static void iree_net_framed_endpoint_reject_send_state(
    iree_net_framed_endpoint_send_state_t* send_state) {
  iree_net_framed_endpoint_t* endpoint = send_state->endpoint;
  iree_slim_mutex_lock(&endpoint->mutex);
  iree_net_framed_endpoint_release_send_state_locked(endpoint, send_state);
  iree_slim_mutex_unlock(&endpoint->mutex);
  iree_net_endpoint_lifecycle_end_operation(&endpoint->lifecycle);
}

static iree_status_t iree_net_framed_endpoint_schedule_local_send_completion(
    iree_net_framed_endpoint_send_state_t* send_state, iree_status_t status) {
  IREE_ASSERT(!iree_status_is_ok(status));
  iree_net_framed_endpoint_t* endpoint = send_state->endpoint;
  send_state->local_completion_status = status;
  iree_async_operation_initialize(
      &send_state->local_completion_operation.base,
      IREE_ASYNC_OPERATION_TYPE_NOP, IREE_ASYNC_OPERATION_FLAG_NONE,
      iree_net_framed_endpoint_local_send_complete, send_state);
  iree_status_t submit_status = iree_async_proactor_submit_one(
      endpoint->proactor, &send_state->local_completion_operation.base);
  if (iree_status_is_ok(submit_status)) {
    return iree_ok_status();
  }

  status = send_state->local_completion_status;
  send_state->local_completion_status = iree_ok_status();
  iree_net_framed_endpoint_reject_send_state(send_state);
  return iree_status_join(status, submit_status);
}

static iree_status_t iree_net_framed_endpoint_send(
    void* self, const iree_net_message_endpoint_send_params_t* params) {
  iree_net_framed_endpoint_t* endpoint = (iree_net_framed_endpoint_t*)self;
  iree_host_size_t payload_length = 0;
  IREE_RETURN_IF_ERROR(iree_net_framed_endpoint_calculate_payload_length(
      params, &payload_length));
  uint32_t frame_length = 0;
  IREE_RETURN_IF_ERROR(iree_net_framed_endpoint_calculate_frame_length(
      payload_length, &frame_length));

  iree_net_framed_endpoint_send_state_t* send_state = NULL;
  IREE_RETURN_IF_ERROR(
      iree_net_framed_endpoint_acquire_send_state(endpoint, &send_state));
  send_state->payload_length = payload_length;
  send_state->completion_callback = params->completion_callback;

  iree_net_framed_endpoint_frame_prefix_t frame_prefix = {
      .frame_length = frame_length,
      .message_prefix = params->generated_prefix,
  };
  iree_net_message_endpoint_send_params_t wire_params = {
      .generated_prefix =
          {
              .length = IREE_NET_FRAMED_ENDPOINT_FRAME_HEADER_SIZE +
                        params->generated_prefix.length,
              .write = iree_net_framed_endpoint_write_frame_prefix,
              .user_data = &frame_prefix,
          },
      .data = params->data,
      .completion_callback =
          {
              .fn = iree_net_framed_endpoint_send_complete,
              .user_data = send_state,
          },
  };
  iree_status_t status =
      iree_net_message_endpoint_send(endpoint->wire_endpoint, &wire_params);
  if (!iree_status_is_ok(status)) {
    return iree_net_framed_endpoint_schedule_local_send_completion(send_state,
                                                                   status);
  }
  return iree_ok_status();
}

static iree_net_carrier_send_budget_t
iree_net_framed_endpoint_query_send_budget(void* self) {
  iree_net_framed_endpoint_t* endpoint = (iree_net_framed_endpoint_t*)self;
  iree_slim_mutex_lock(&endpoint->mutex);
  iree_net_carrier_send_budget_t budget = {0};
  if (endpoint->state == IREE_NET_FRAMED_ENDPOINT_STATE_ACTIVE &&
      endpoint->free_send_state_count > 0) {
    budget =
        iree_net_message_endpoint_query_send_budget(endpoint->wire_endpoint);
    budget.slots = iree_min(budget.slots, endpoint->free_send_state_count);
    if (budget.bytes != IREE_HOST_SIZE_MAX) {
      budget.bytes =
          budget.bytes > IREE_NET_FRAMED_ENDPOINT_FRAME_HEADER_SIZE
              ? budget.bytes - IREE_NET_FRAMED_ENDPOINT_FRAME_HEADER_SIZE
              : 0;
    }
  }
  iree_slim_mutex_unlock(&endpoint->mutex);
  return budget;
}

static const iree_net_message_endpoint_vtable_t
    iree_net_framed_endpoint_vtable = {
        .set_callbacks = iree_net_framed_endpoint_set_callbacks,
        .activate = iree_net_framed_endpoint_activate,
        .deactivate = iree_net_framed_endpoint_deactivate,
        .send = iree_net_framed_endpoint_send,
        .query_send_budget = iree_net_framed_endpoint_query_send_budget,
        .receive = iree_net_framed_endpoint_receive,
};

iree_status_t iree_net_framed_endpoint_allocate(
    iree_net_carrier_t* carrier, iree_async_proactor_t* proactor,
    uint32_t max_send_operations,
    iree_net_endpoint_deactivation_barrier_t* connection_barrier,
    iree_allocator_t host_allocator,
    iree_net_framed_endpoint_t** out_endpoint) {
  IREE_ASSERT_ARGUMENT(out_endpoint);
  *out_endpoint = NULL;
  if (!carrier || !proactor || max_send_operations == 0) {
    return iree_make_status(
        IREE_STATUS_INVALID_ARGUMENT,
        "carrier, proactor, and nonzero send operation limit are required");
  }

  iree_host_size_t allocation_size = 0;
  IREE_RETURN_IF_ERROR(IREE_STRUCT_LAYOUT(
      sizeof(iree_net_framed_endpoint_t), &allocation_size,
      IREE_STRUCT_FIELD_FAM(max_send_operations,
                            iree_net_framed_endpoint_send_state_t)));
  iree_net_framed_endpoint_t* endpoint = NULL;
  IREE_RETURN_IF_ERROR(iree_allocator_malloc(host_allocator, allocation_size,
                                             (void**)&endpoint));
  memset(endpoint, 0, allocation_size);
  iree_slim_mutex_initialize(&endpoint->mutex);
  endpoint->proactor = proactor;
  iree_async_proactor_retain(proactor);
  iree_net_endpoint_lifecycle_initialize(connection_barrier,
                                         &endpoint->lifecycle);
  iree_net_message_receive_queue_initialize(&endpoint->lifecycle,
                                            &endpoint->receive_queue);
  endpoint->state = IREE_NET_FRAMED_ENDPOINT_STATE_CREATED;
  endpoint->host_allocator = host_allocator;
  endpoint->send_state_count = max_send_operations;
  endpoint->free_send_state_count = max_send_operations;
  endpoint->free_send_state_head = 0;
  for (uint32_t i = 0; i < max_send_operations; ++i) {
    iree_net_framed_endpoint_send_state_t* send_state =
        &endpoint->send_states[i];
    send_state->endpoint = endpoint;
    send_state->index = i;
    send_state->next_free = i + 1 < max_send_operations
                                ? i + 1
                                : IREE_NET_FRAMED_ENDPOINT_SEND_STATE_NONE;
  }

  iree_net_frame_length_callback_t frame_length = {
      .fn = iree_net_framed_endpoint_resolve_frame_length,
      .user_data = NULL,
      .header_size = IREE_NET_FRAMED_ENDPOINT_FRAME_HEADER_SIZE,
  };
  iree_status_t status = iree_net_framing_adapter_allocate(
      carrier, frame_length, UINT32_MAX, connection_barrier, host_allocator,
      &endpoint->framing_adapter);
  if (iree_status_is_ok(status)) {
    endpoint->wire_endpoint =
        iree_net_framing_adapter_as_endpoint(endpoint->framing_adapter);
    iree_net_framing_adapter_set_receive_router(
        endpoint->framing_adapter,
        (iree_net_frame_receive_router_t){
            .fn = iree_net_framed_endpoint_route_receive,
            .user_data = endpoint});
    iree_net_message_endpoint_set_callbacks(
        endpoint->wire_endpoint,
        (iree_net_message_endpoint_callbacks_t){
            .on_message = iree_net_framed_endpoint_on_wire_message,
            .on_error = iree_net_framed_endpoint_on_wire_error,
            .user_data = endpoint,
        });
    *out_endpoint = endpoint;
  } else {
    iree_net_message_receive_queue_deinitialize(&endpoint->receive_queue);
    iree_net_endpoint_lifecycle_deinitialize(&endpoint->lifecycle);
    iree_async_proactor_release(endpoint->proactor);
    iree_slim_mutex_deinitialize(&endpoint->mutex);
    iree_allocator_free(host_allocator, endpoint);
  }
  return status;
}

void iree_net_framed_endpoint_free(iree_net_framed_endpoint_t* endpoint) {
  if (!endpoint) {
    return;
  }
  IREE_ASSERT(endpoint->state != IREE_NET_FRAMED_ENDPOINT_STATE_ACTIVE,
              "active framed endpoint cannot be freed");
  IREE_ASSERT(endpoint->free_send_state_count == endpoint->send_state_count,
              "framed endpoint freed with owned send state");
  iree_allocator_t host_allocator = endpoint->host_allocator;
  iree_net_framing_adapter_free(endpoint->framing_adapter);
  iree_net_message_receive_queue_deinitialize(&endpoint->receive_queue);
  iree_net_endpoint_lifecycle_deinitialize(&endpoint->lifecycle);
  iree_async_proactor_release(endpoint->proactor);
  iree_slim_mutex_deinitialize(&endpoint->mutex);
  iree_allocator_free(host_allocator, endpoint);
}

void iree_net_framed_endpoint_join_deactivation(
    iree_net_framed_endpoint_t* endpoint) {
  IREE_ASSERT_ARGUMENT(endpoint);
  iree_net_endpoint_lifecycle_actions_t actions =
      IREE_NET_ENDPOINT_LIFECYCLE_ACTION_NONE;
  iree_slim_mutex_lock(&endpoint->mutex);
  if (endpoint->state == IREE_NET_FRAMED_ENDPOINT_STATE_ACTIVE) {
    actions =
        iree_net_endpoint_lifecycle_join_deactivation(&endpoint->lifecycle);
    endpoint->state = IREE_NET_FRAMED_ENDPOINT_STATE_DRAINING;
  }
  iree_slim_mutex_unlock(&endpoint->mutex);
  iree_net_message_receive_queue_close(&endpoint->receive_queue,
                                       iree_ok_status());
  iree_net_framing_adapter_join_deactivation(endpoint->framing_adapter);
  if (iree_any_bit_set(actions,
                       IREE_NET_ENDPOINT_LIFECYCLE_ACTION_BEGIN_DEACTIVATION)) {
    iree_net_endpoint_lifecycle_complete_deactivation(&endpoint->lifecycle);
  }
}

iree_net_message_endpoint_t iree_net_framed_endpoint_as_message_endpoint(
    iree_net_framed_endpoint_t* endpoint) {
  IREE_ASSERT_ARGUMENT(endpoint);
  return (iree_net_message_endpoint_t){
      .self = endpoint,
      .vtable = &iree_net_framed_endpoint_vtable,
  };
}
