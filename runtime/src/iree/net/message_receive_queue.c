// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/net/message_receive_queue.h"

#include <string.h>

#include "iree/net/endpoint_lifecycle.h"

void iree_net_message_receive_queue_initialize(
    iree_net_endpoint_lifecycle_t* lifecycle,
    iree_net_message_receive_queue_t* out_queue) {
  memset(out_queue, 0, sizeof(*out_queue));
  iree_slim_mutex_initialize(&out_queue->mutex);
  out_queue->lifecycle = lifecycle;
}

void iree_net_message_receive_queue_deinitialize(
    iree_net_message_receive_queue_t* queue) {
  IREE_ASSERT(!queue->head && !queue->active,
              "receive destinations must retire before their owner");
  iree_status_free(queue->status);
  iree_slim_mutex_deinitialize(&queue->mutex);
}

iree_status_t iree_net_message_receive_queue_submit(
    iree_net_message_receive_queue_t* queue,
    iree_net_message_receive_operation_t* operation,
    const iree_net_message_endpoint_receive_params_t* params) {
  int32_t expected = 0;
  if (!iree_atomic_compare_exchange_strong(&operation->claimed, &expected, 1,
                                           iree_memory_order_acq_rel,
                                           iree_memory_order_acquire)) {
    return iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                            "message receive operation is already admitted");
  }

  iree_status_t status = iree_ok_status();
  iree_slim_mutex_lock(&queue->mutex);
  if (queue->closed) {
    status = iree_status_clone(queue->status);
  } else if (queue->prefix_length &&
             queue->prefix_length != params->prefix.data_length) {
    status = iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                              "endpoint receive prefix widths must agree");
  }
  for (iree_net_message_receive_operation_t* pending = queue->head;
       pending && iree_status_is_ok(status); pending = pending->next) {
    if (memcmp(pending->prefix, params->prefix.data,
               params->prefix.data_length) == 0) {
      status = iree_make_status(IREE_STATUS_ALREADY_EXISTS,
                                "message receive prefix is already posted");
    }
  }
  if (iree_status_is_ok(status) && queue->active &&
      memcmp(queue->active->prefix, params->prefix.data,
             params->prefix.data_length) == 0) {
    status = iree_make_status(IREE_STATUS_ALREADY_EXISTS,
                              "message receive prefix is already active");
  }
  if (iree_status_is_ok(status) &&
      !iree_net_endpoint_lifecycle_try_begin_operation(queue->lifecycle)) {
    status = iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                              "message endpoint receive admission is closed");
  }
  if (iree_status_is_ok(status)) {
    operation->queue = queue;
    operation->data = params->data;
    operation->completion_callback = params->completion_callback;
    memcpy(operation->prefix, params->prefix.data, params->prefix.data_length);
    operation->next = queue->head;
    queue->head = operation;
    queue->prefix_length = params->prefix.data_length;
  } else {
    iree_atomic_store(&operation->claimed, 0, iree_memory_order_release);
  }
  iree_slim_mutex_unlock(&queue->mutex);
  return status;
}

iree_host_size_t iree_net_message_receive_queue_prefix_length(
    iree_net_message_receive_queue_t* queue) {
  iree_slim_mutex_lock(&queue->mutex);
  const iree_host_size_t prefix_length = queue->prefix_length;
  iree_slim_mutex_unlock(&queue->mutex);
  return prefix_length;
}

iree_status_t iree_net_message_receive_queue_match(
    iree_net_message_receive_queue_t* queue, iree_const_byte_span_t prefix,
    iree_host_size_t body_length,
    iree_net_message_receive_operation_t** out_operation) {
  *out_operation = NULL;
  iree_status_t status = iree_ok_status();
  iree_slim_mutex_lock(&queue->mutex);
  if (!queue->closed && prefix.data_length == queue->prefix_length) {
    iree_net_message_receive_operation_t** link = &queue->head;
    while (*link &&
           memcmp((*link)->prefix, prefix.data, prefix.data_length) != 0) {
      link = &(*link)->next;
    }
    if (*link) {
      iree_net_message_receive_operation_t* operation = *link;
      if (operation->data.data_length != body_length) {
        status = iree_make_status(IREE_STATUS_DATA_LOSS,
                                  "message body extent %" PRIhsz
                                  " differs from posted %" PRIhsz,
                                  body_length, operation->data.data_length);
      } else {
        *link = operation->next;
        operation->next = NULL;
        queue->active = operation;
        *out_operation = operation;
      }
    }
  }
  iree_slim_mutex_unlock(&queue->mutex);
  return status;
}

void iree_net_message_receive_queue_complete(
    iree_net_message_receive_operation_t* operation, iree_status_t status,
    iree_host_size_t bytes_transferred) {
  iree_net_message_receive_queue_t* queue = operation->queue;
  iree_net_endpoint_lifecycle_t* lifecycle = queue->lifecycle;
  const iree_net_receive_completion_callback_t callback =
      operation->completion_callback;
  iree_slim_mutex_lock(&queue->mutex);
  if (queue->active == operation) {
    queue->active = NULL;
  }
  status = iree_status_join(status, iree_status_clone(queue->status));
  operation->queue = NULL;
  iree_atomic_store(&operation->claimed, 0, iree_memory_order_release);
  iree_slim_mutex_unlock(&queue->mutex);
  callback.fn(callback.user_data, status, bytes_transferred);
  iree_net_endpoint_lifecycle_end_operation(lifecycle);
}

void iree_net_message_receive_queue_close(
    iree_net_message_receive_queue_t* queue, iree_status_t status) {
  iree_slim_mutex_lock(&queue->mutex);
  queue->closed = true;
  queue->status = iree_status_join(queue->status, status);
  if (iree_status_is_ok(queue->status)) {
    queue->status = iree_status_from_code(IREE_STATUS_CANCELLED);
  }
  iree_net_message_receive_operation_t* pending = queue->head;
  queue->head = NULL;
  iree_slim_mutex_unlock(&queue->mutex);
  while (pending) {
    iree_net_message_receive_operation_t* next = pending->next;
    pending->next = NULL;
    iree_net_message_receive_queue_complete(pending, iree_ok_status(), 0);
    pending = next;
  }
}
