// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/net/rdma/device_events.h"

#include <fcntl.h>

#include "iree/base/threading/mutex.h"

struct iree_net_rdma_device_events_t {
  // Allocator for this object.
  iree_allocator_t host_allocator;
  // Borrowed symbols retained by the enclosing native context.
  const iree_net_rdma_library_t* library;
  // Borrowed, independently opened native context and event stream.
  struct ibv_context* device;
  // Selected one-based port for link failure routing.
  uint8_t port_number;
  // Serializes event acknowledgment, subscription lifetime, and error handoff.
  iree_slim_mutex_t mutex;
  // Explicit CQ-scoped subscriptions, traversed only for rare native errors.
  iree_net_rdma_device_event_subscription_t* subscriptions;
  // Owned device-wide terminal error, also delivered to new subscribers.
  iree_status_t failure;
};

iree_status_t iree_net_rdma_device_events_create(
    const iree_net_rdma_library_t* library, struct ibv_context* device,
    uint8_t port_number, iree_allocator_t host_allocator,
    iree_net_rdma_device_events_t** out_events) {
  *out_events = NULL;
  int flags = fcntl(device->async_fd, F_GETFL);
  if (flags < 0 || fcntl(device->async_fd, F_SETFL, flags | O_NONBLOCK)) {
    int error = errno;
    return iree_make_status(iree_status_code_from_errno(error),
                            "RDMA async event flags: %s", strerror(error));
  }
  iree_net_rdma_device_events_t* events = NULL;
  IREE_RETURN_IF_ERROR(
      iree_allocator_malloc(host_allocator, sizeof(*events), (void**)&events));
  events->host_allocator = host_allocator;
  events->library = library;
  events->device = device;
  events->port_number = port_number;
  iree_slim_mutex_initialize(&events->mutex);
  *out_events = events;
  return iree_ok_status();
}

void iree_net_rdma_device_events_destroy(
    iree_net_rdma_device_events_t* events) {
  if (!events) {
    return;
  }
  IREE_ASSERT(!events->subscriptions,
              "native event subscriptions must retire before their owner");
  iree_status_free(events->failure);
  iree_slim_mutex_deinitialize(&events->mutex);
  iree_allocator_free(events->host_allocator, events);
}

static void iree_net_rdma_device_events_notify(
    iree_net_rdma_device_event_subscription_t* subscription,
    iree_status_t status) {
  void (*on_failure)(void*, iree_status_t) = subscription->on_failure;
  subscription->on_failure = NULL;
  on_failure(subscription->user_data, status);
}

void iree_net_rdma_device_events_subscribe(
    iree_net_rdma_device_events_t* events, struct ibv_cq* queue,
    void (*on_failure)(void*, iree_status_t), void* user_data,
    iree_net_rdma_device_event_subscription_t* subscription) {
  iree_slim_mutex_lock(&events->mutex);
  *subscription = (iree_net_rdma_device_event_subscription_t){
      .next = events->subscriptions,
      .queue = queue,
      .on_failure = on_failure,
      .user_data = user_data,
  };
  events->subscriptions = subscription;
  if (!iree_status_is_ok(events->failure)) {
    iree_net_rdma_device_events_notify(subscription,
                                       iree_status_clone(events->failure));
  }
  iree_slim_mutex_unlock(&events->mutex);
}

void iree_net_rdma_device_events_unsubscribe(
    iree_net_rdma_device_events_t* events,
    iree_net_rdma_device_event_subscription_t* subscription) {
  iree_slim_mutex_lock(&events->mutex);
  iree_net_rdma_device_event_subscription_t** link = &events->subscriptions;
  while (*link != subscription) {
    link = &(*link)->next;
  }
  *link = subscription->next;
  iree_slim_mutex_unlock(&events->mutex);
}

static void iree_net_rdma_device_events_fail_locked(
    iree_net_rdma_device_events_t* events, iree_status_t status) {
  events->failure = iree_status_join(events->failure, status);
  for (iree_net_rdma_device_event_subscription_t* subscription =
           events->subscriptions;
       subscription; subscription = subscription->next) {
    if (subscription->on_failure) {
      iree_net_rdma_device_events_notify(subscription,
                                         iree_status_clone(events->failure));
    }
  }
}

void iree_net_rdma_device_events_fail(iree_net_rdma_device_events_t* events,
                                      iree_status_t status) {
  iree_slim_mutex_lock(&events->mutex);
  iree_net_rdma_device_events_fail_locked(events, status);
  iree_slim_mutex_unlock(&events->mutex);
}

static void iree_net_rdma_device_events_dispatch(
    iree_net_rdma_device_events_t* events, struct ibv_async_event* event) {
  // Read native object relationships before acknowledgment allows concurrent
  // destruction. Subscription ownership keeps CQ identities stable afterward.
  struct ibv_cq* send_queue = NULL;
  struct ibv_cq* receive_queue = NULL;
  bool device_failure = false;
  enum ibv_event_type type = event->event_type;
  switch (type) {
    case IBV_EVENT_CQ_ERR:
      send_queue = event->element.cq;
      break;
    case IBV_EVENT_QP_FATAL:
    case IBV_EVENT_QP_REQ_ERR:
    case IBV_EVENT_QP_ACCESS_ERR:
    case IBV_EVENT_PATH_MIG_ERR:
      send_queue = event->element.qp->send_cq;
      receive_queue = event->element.qp->recv_cq;
      break;
    case IBV_EVENT_WQ_FATAL:
      receive_queue = event->element.wq->cq;
      break;
    case IBV_EVENT_PORT_ERR:
      device_failure = event->element.port_num == events->port_number;
      break;
    case IBV_EVENT_DEVICE_FATAL:
    case IBV_EVENT_SRQ_ERR:
      device_failure = true;
      break;
    default:
      break;
  }
  events->library->ibv_ack_async_event(event);
  if (device_failure) {
    iree_net_rdma_device_events_fail_locked(
        events,
        iree_make_status(IREE_STATUS_UNAVAILABLE,
                         "RDMA device asynchronous event %u", (unsigned)type));
  } else if (send_queue || receive_queue) {
    for (iree_net_rdma_device_event_subscription_t* subscription =
             events->subscriptions;
         subscription; subscription = subscription->next) {
      if (subscription->on_failure && (subscription->queue == send_queue ||
                                       subscription->queue == receive_queue)) {
        iree_net_rdma_device_events_notify(
            subscription, iree_make_status(IREE_STATUS_UNAVAILABLE,
                                           "RDMA queue asynchronous event %u",
                                           (unsigned)type));
      }
    }
  }
}

bool iree_net_rdma_device_events_poll(iree_net_rdma_device_events_t* events,
                                      uint32_t limit,
                                      uint32_t* out_completed_count) {
  *out_completed_count = 0;
  iree_slim_mutex_lock(&events->mutex);
  uint32_t attempt_count = 0;
  for (; attempt_count < limit && iree_status_is_ok(events->failure);
       ++attempt_count) {
    struct ibv_async_event event;
    if (events->library->ibv_get_async_event(events->device, &event)) {
      int error = errno;
      if (error == EAGAIN) {
        break;
      }
      if (error == EINTR) {
        continue;
      }
      iree_net_rdma_device_events_fail_locked(
          events, iree_make_status(iree_status_code_from_errno(error),
                                   "ibv_get_async_event: %s", strerror(error)));
    } else {
      iree_net_rdma_device_events_dispatch(events, &event);
      ++*out_completed_count;
    }
  }
  bool has_more = attempt_count == limit && iree_status_is_ok(events->failure);
  iree_slim_mutex_unlock(&events->mutex);
  return has_more;
}
