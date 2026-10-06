// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/net/carrier/loopback/connection.h"

#include <string.h>

#include "iree/async/operations/scheduling.h"
#include "iree/base/threading/mutex.h"
#include "iree/net/endpoint_lifecycle.h"
#include "iree/net/framed_endpoint.h"

typedef struct iree_net_loopback_connection_t iree_net_loopback_connection_t;

typedef enum iree_net_loopback_connection_state_e {
  // Endpoint opens may be accepted.
  IREE_NET_LOOPBACK_CONNECTION_STATE_OPEN = 0,
  // Deactivation is waiting for accepted ready callbacks.
  IREE_NET_LOOPBACK_CONNECTION_STATE_DRAINING_READY_CALLBACKS = 1,
  // Endpoint carrier drains have started.
  IREE_NET_LOOPBACK_CONNECTION_STATE_DRAINING_ENDPOINTS = 2,
  // Every endpoint has drained.
  IREE_NET_LOOPBACK_CONNECTION_STATE_DEACTIVATED = 3,
} iree_net_loopback_connection_state_t;

typedef struct iree_net_loopback_endpoint_slot_t {
  // Connection owning this ordinal slot.
  iree_net_loopback_connection_t* connection;

  // Eagerly constructed payload endpoint for this ordinal.
  iree_net_framed_endpoint_t* endpoint;

  // Preallocated operation dispatching the endpoint-ready callback.
  iree_async_nop_operation_t ready_operation;

  // Callback owned while |ready_operation| is submitted.
  iree_net_endpoint_ready_callback_t ready_callback;
} iree_net_loopback_endpoint_slot_t;

struct iree_net_loopback_connection_t {
  // Public connection base; must be first.
  iree_net_connection_t base;

  // Serializes endpoint claims and connection lifecycle transitions.
  iree_slim_mutex_t mutex;

  // Proactor dispatching endpoint-ready callbacks. Retained.
  iree_async_proactor_t* proactor;

  // Current connection lifecycle state.
  iree_net_loopback_connection_state_t state;

  // True after ownership has transferred through a public callback.
  bool published;

  // Number of monotonically claimed endpoint ordinals.
  uint32_t opened_endpoint_count;

  // Number of accepted endpoint-ready operations not yet retired.
  uint32_t pending_ready_count;

  // Callback awaiting complete connection deactivation.
  iree_net_connection_deactivate_callback_t deactivate_callback;

  // Embedded barrier joining every endpoint carrier drain.
  iree_net_endpoint_deactivation_barrier_t deactivation_barrier;

  // Eager endpoint slots indexed by protocol ordinal.
  iree_net_loopback_endpoint_slot_t endpoints[];
};

static const iree_net_connection_vtable_t iree_net_loopback_connection_vtable;

static void iree_net_loopback_connection_destroy(
    iree_net_connection_t* base_connection) {
  iree_net_loopback_connection_t* connection =
      (iree_net_loopback_connection_t*)base_connection;
  IREE_ASSERT(
      !connection->published ||
          connection->state == IREE_NET_LOOPBACK_CONNECTION_STATE_DEACTIVATED,
      "published loopback connection released before deactivation");
  IREE_ASSERT(connection->pending_ready_count == 0,
              "loopback connection destroyed with pending ready callbacks");

  iree_allocator_t host_allocator = connection->base.host_allocator;
  for (uint32_t i = 0; i < connection->base.max_endpoint_count; ++i) {
    iree_net_framed_endpoint_free(connection->endpoints[i].endpoint);
  }
  iree_async_proactor_release(connection->proactor);
  iree_slim_mutex_deinitialize(&connection->mutex);
  iree_allocator_free(host_allocator, connection);
}

static iree_status_t iree_net_loopback_connection_create(
    iree_async_proactor_t* proactor, uint32_t max_endpoint_count,
    iree_allocator_t host_allocator,
    iree_net_loopback_connection_t** out_connection) {
  *out_connection = NULL;
  iree_host_size_t allocation_size = 0;
  IREE_RETURN_IF_ERROR(IREE_STRUCT_LAYOUT(
      sizeof(iree_net_loopback_connection_t), &allocation_size,
      IREE_STRUCT_FIELD_FAM(max_endpoint_count,
                            iree_net_loopback_endpoint_slot_t)));
  iree_net_loopback_connection_t* connection = NULL;
  IREE_RETURN_IF_ERROR(iree_allocator_malloc(host_allocator, allocation_size,
                                             (void**)&connection));
  memset(connection, 0, allocation_size);
  iree_net_connection_initialize(&iree_net_loopback_connection_vtable,
                                 host_allocator, max_endpoint_count,
                                 &connection->base);
  iree_slim_mutex_initialize(&connection->mutex);
  iree_net_endpoint_deactivation_barrier_initialize(
      &connection->deactivation_barrier);
  connection->proactor = proactor;
  iree_async_proactor_retain(proactor);
  connection->state = IREE_NET_LOOPBACK_CONNECTION_STATE_OPEN;
  for (uint32_t i = 0; i < max_endpoint_count; ++i) {
    connection->endpoints[i].connection = connection;
  }
  *out_connection = connection;
  return iree_ok_status();
}

static void iree_net_loopback_connection_deactivation_complete(
    void* user_data) {
  iree_net_loopback_connection_t* connection =
      (iree_net_loopback_connection_t*)user_data;
  iree_slim_mutex_lock(&connection->mutex);
  IREE_ASSERT(connection->state ==
                  IREE_NET_LOOPBACK_CONNECTION_STATE_DRAINING_ENDPOINTS,
              "loopback connection drain completed from state %d",
              (int)connection->state);
  connection->state = IREE_NET_LOOPBACK_CONNECTION_STATE_DEACTIVATED;
  iree_net_connection_deactivate_callback_t callback =
      connection->deactivate_callback;
  connection->deactivate_callback =
      (iree_net_connection_deactivate_callback_t){0};
  iree_slim_mutex_unlock(&connection->mutex);

  callback.fn(callback.user_data);
  iree_net_connection_release(&connection->base);
}

static void iree_net_loopback_connection_begin_endpoint_drain(
    iree_net_loopback_connection_t* connection) {
  for (uint32_t i = 0; i < connection->base.max_endpoint_count; ++i) {
    iree_net_framed_endpoint_join_deactivation(
        connection->endpoints[i].endpoint);
  }
  iree_net_endpoint_deactivation_barrier_commit(
      &connection->deactivation_barrier,
      (iree_net_connection_deactivate_callback_t){
          .fn = iree_net_loopback_connection_deactivation_complete,
          .user_data = connection,
      });
}

static void iree_net_loopback_connection_deactivate(
    iree_net_connection_t* base_connection,
    iree_net_connection_deactivate_callback_t callback) {
  iree_net_loopback_connection_t* connection =
      (iree_net_loopback_connection_t*)base_connection;
  bool begin_endpoint_drain = false;
  bool valid_request = false;
  iree_slim_mutex_lock(&connection->mutex);
  if (connection->published &&
      connection->state == IREE_NET_LOOPBACK_CONNECTION_STATE_OPEN) {
    valid_request = true;
    connection->deactivate_callback = callback;
    iree_net_connection_retain(base_connection);
    if (connection->pending_ready_count == 0) {
      connection->state = IREE_NET_LOOPBACK_CONNECTION_STATE_DRAINING_ENDPOINTS;
      begin_endpoint_drain = true;
    } else {
      connection->state =
          IREE_NET_LOOPBACK_CONNECTION_STATE_DRAINING_READY_CALLBACKS;
    }
  }
  iree_slim_mutex_unlock(&connection->mutex);

  IREE_ASSERT(valid_request,
              "loopback connection deactivated from an invalid state");
  if (begin_endpoint_drain) {
    iree_net_loopback_connection_begin_endpoint_drain(connection);
  }
}

static void iree_net_loopback_endpoint_ready_complete(
    void* user_data, iree_async_operation_t* operation, iree_status_t status,
    iree_async_completion_flags_t flags) {
  (void)operation;
  IREE_ASSERT(!iree_any_bit_set(flags, IREE_ASYNC_COMPLETION_FLAG_MORE),
              "endpoint-ready NOP produced a nonterminal completion");
  iree_net_loopback_endpoint_slot_t* slot =
      (iree_net_loopback_endpoint_slot_t*)user_data;
  iree_net_loopback_connection_t* connection = slot->connection;

  iree_net_message_endpoint_t message_endpoint = {0};
  iree_slim_mutex_lock(&connection->mutex);
  iree_net_endpoint_ready_callback_t ready_callback = slot->ready_callback;
  slot->ready_callback = (iree_net_endpoint_ready_callback_t){0};
  if (iree_status_is_ok(status) &&
      connection->state == IREE_NET_LOOPBACK_CONNECTION_STATE_OPEN) {
    message_endpoint =
        iree_net_framed_endpoint_as_message_endpoint(slot->endpoint);
  } else if (iree_status_is_ok(status)) {
    status = iree_make_status(IREE_STATUS_CANCELLED,
                              "connection deactivated before endpoint ready");
  }
  iree_slim_mutex_unlock(&connection->mutex);

  ready_callback.fn(ready_callback.user_data, status, message_endpoint);

  bool begin_endpoint_drain = false;
  iree_slim_mutex_lock(&connection->mutex);
  IREE_ASSERT(connection->pending_ready_count > 0,
              "loopback connection retired an unowned ready callback");
  --connection->pending_ready_count;
  if (connection->pending_ready_count == 0 &&
      connection->state ==
          IREE_NET_LOOPBACK_CONNECTION_STATE_DRAINING_READY_CALLBACKS) {
    connection->state = IREE_NET_LOOPBACK_CONNECTION_STATE_DRAINING_ENDPOINTS;
    begin_endpoint_drain = true;
  }
  iree_slim_mutex_unlock(&connection->mutex);

  if (begin_endpoint_drain) {
    iree_net_loopback_connection_begin_endpoint_drain(connection);
  }
  iree_net_connection_release(&connection->base);
}

static iree_status_t iree_net_loopback_connection_open_endpoint(
    iree_net_connection_t* base_connection,
    iree_net_endpoint_ready_callback_t callback) {
  iree_net_loopback_connection_t* connection =
      (iree_net_loopback_connection_t*)base_connection;
  iree_slim_mutex_lock(&connection->mutex);
  iree_status_t status = iree_ok_status();
  iree_net_loopback_endpoint_slot_t* slot = NULL;
  if (!connection->published) {
    status = iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                              "loopback connection is not published");
  } else if (connection->state != IREE_NET_LOOPBACK_CONNECTION_STATE_OPEN) {
    status = iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                              "loopback connection is deactivating");
  } else if (connection->opened_endpoint_count >=
             connection->base.max_endpoint_count) {
    status = iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                              "all %u endpoint slots are claimed",
                              connection->base.max_endpoint_count);
  } else {
    slot = &connection->endpoints[connection->opened_endpoint_count];
    slot->ready_callback = callback;
    iree_async_operation_initialize(
        &slot->ready_operation.base, IREE_ASYNC_OPERATION_TYPE_NOP,
        IREE_ASYNC_OPERATION_FLAG_NONE,
        iree_net_loopback_endpoint_ready_complete, slot);
    ++connection->pending_ready_count;
    iree_net_connection_retain(base_connection);
    status = iree_async_proactor_submit_one(connection->proactor,
                                            &slot->ready_operation.base);
    if (iree_status_is_ok(status)) {
      ++connection->opened_endpoint_count;
    } else {
      --connection->pending_ready_count;
      slot->ready_callback = (iree_net_endpoint_ready_callback_t){0};
    }
  }
  iree_slim_mutex_unlock(&connection->mutex);
  if (slot && !iree_status_is_ok(status)) {
    iree_net_connection_release(base_connection);
  }
  return status;
}

static iree_async_proactor_t* iree_net_loopback_connection_proactor(
    iree_net_connection_t* base_connection) {
  iree_net_loopback_connection_t* connection =
      (iree_net_loopback_connection_t*)base_connection;
  return connection->proactor;
}

static const iree_net_connection_vtable_t iree_net_loopback_connection_vtable =
    {
        .destroy = iree_net_loopback_connection_destroy,
        .deactivate = iree_net_loopback_connection_deactivate,
        .open_endpoint = iree_net_loopback_connection_open_endpoint,
        .open_direct_endpoint = NULL,
        .proactor = iree_net_loopback_connection_proactor,
};

iree_status_t iree_net_loopback_connection_create_pair(
    iree_async_proactor_t* client_proactor,
    iree_async_proactor_t* server_proactor, uint32_t max_endpoint_count,
    const iree_net_loopback_carrier_options_t* carrier_options,
    iree_allocator_t host_allocator, iree_net_connection_t** out_client,
    iree_net_connection_t** out_server) {
  IREE_ASSERT_ARGUMENT(client_proactor);
  IREE_ASSERT_ARGUMENT(server_proactor);
  IREE_ASSERT_ARGUMENT(out_client);
  IREE_ASSERT_ARGUMENT(out_server);
  *out_client = NULL;
  *out_server = NULL;
  if (max_endpoint_count == 0) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "loopback connection requires endpoint slots");
  }
  iree_net_loopback_carrier_options_t default_carrier_options =
      iree_net_loopback_carrier_options_default();
  if (!carrier_options) {
    carrier_options = &default_carrier_options;
  }
  if (carrier_options->max_send_operations == 0 ||
      carrier_options->max_send_spans == 0) {
    return iree_make_status(
        IREE_STATUS_INVALID_ARGUMENT,
        "loopback send operation and span limits must be nonzero");
  }

  iree_net_loopback_connection_t* client_connection = NULL;
  iree_net_loopback_connection_t* server_connection = NULL;
  iree_status_t status = iree_net_loopback_connection_create(
      client_proactor, max_endpoint_count, host_allocator, &client_connection);
  if (iree_status_is_ok(status)) {
    status =
        iree_net_loopback_connection_create(server_proactor, max_endpoint_count,
                                            host_allocator, &server_connection);
  }

  for (uint32_t i = 0; i < max_endpoint_count && iree_status_is_ok(status);
       ++i) {
    iree_net_carrier_t* client_carrier = NULL;
    iree_net_carrier_t* server_carrier = NULL;
    status = iree_net_loopback_carrier_create_pair(
        client_proactor, server_proactor, carrier_options, host_allocator,
        &client_carrier, &server_carrier);
    if (iree_status_is_ok(status)) {
      status = iree_net_framed_endpoint_allocate(
          client_carrier, client_proactor, carrier_options->max_send_operations,
          &client_connection->deactivation_barrier, host_allocator,
          &client_connection->endpoints[i].endpoint);
      if (iree_status_is_ok(status)) {
        client_carrier = NULL;
      }
    }
    if (iree_status_is_ok(status)) {
      status = iree_net_framed_endpoint_allocate(
          server_carrier, server_proactor, carrier_options->max_send_operations,
          &server_connection->deactivation_barrier, host_allocator,
          &server_connection->endpoints[i].endpoint);
      if (iree_status_is_ok(status)) {
        server_carrier = NULL;
      }
    }
    iree_net_carrier_release(client_carrier);
    iree_net_carrier_release(server_carrier);
  }

  if (iree_status_is_ok(status)) {
    *out_client = &client_connection->base;
    *out_server = &server_connection->base;
  } else {
    iree_net_connection_release(client_connection ? &client_connection->base
                                                  : NULL);
    iree_net_connection_release(server_connection ? &server_connection->base
                                                  : NULL);
  }
  return status;
}

void iree_net_loopback_connection_publish(
    iree_net_connection_t* base_connection) {
  IREE_ASSERT_ARGUMENT(base_connection);
  iree_net_loopback_connection_t* connection =
      (iree_net_loopback_connection_t*)base_connection;
  iree_slim_mutex_lock(&connection->mutex);
  IREE_ASSERT(!connection->published,
              "loopback connection published more than once");
  connection->published = true;
  iree_slim_mutex_unlock(&connection->mutex);
}
