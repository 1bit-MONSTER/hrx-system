// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef IREE_NET_RDMA_DEVICE_EVENTS_H_
#define IREE_NET_RDMA_DEVICE_EVENTS_H_

#include "iree/net/rdma/library.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

// Serializes the async event stream of one independently opened verbs context.
// This is a rare-error path, independent of transfer posting and completion.
// The native context and library outlive this owner. No thread or poll owner is
// created; callers service readiness on the native context's async_fd.
typedef struct iree_net_rdma_device_events_t iree_net_rdma_device_events_t;

// Caller-owned subscription, stable until unsubscribe returns. All fields are
// managed by the event owner. One CQ may have several explicit subscribers.
typedef struct iree_net_rdma_device_event_subscription_t {
  // Next subscription on this explicit context, never a process-global list.
  struct iree_net_rdma_device_event_subscription_t* next;
  // Borrowed CQ identity; unsubscribe before destroying the CQ.
  struct ibv_cq* queue;
  // One-shot error handoff, cleared after its first invocation.
  void (*on_failure)(void* user_data, iree_status_t status);
  // Borrowed handoff owner, valid until unsubscribe returns.
  void* user_data;
} iree_net_rdma_device_event_subscription_t;

// Creates an owner and makes the exclusively owned native async_fd nonblocking.
// Native/allocator failures leave output NULL. The selected port scopes link
// failures; device failures affect every subscription.
IREE_API_EXPORT iree_status_t iree_net_rdma_device_events_create(
    const iree_net_rdma_library_t* library, struct ibv_context* device,
    uint8_t port_number, iree_allocator_t host_allocator,
    iree_net_rdma_device_events_t** out_events);

// Destroys an owner after every subscription and readiness monitor has joined.
IREE_API_EXPORT void iree_net_rdma_device_events_destroy(
    iree_net_rdma_device_events_t* events);

// Subscribes to CQ errors, errors on QPs using the CQ, and device/port
// failures. The callback receives an owned status at most once, on the first
// failure, possibly inline if the device has already failed. It runs under the
// event-owner lock on whichever thread services the device: it must only
// publish asynchronous owner work, never unsubscribe, wait for another thread,
// or invoke application callbacks. Every obtained native event is acknowledged
// before any such handoff.
IREE_API_EXPORT void iree_net_rdma_device_events_subscribe(
    iree_net_rdma_device_events_t* events, struct ibv_cq* queue,
    void (*on_failure)(void* user_data, iree_status_t status), void* user_data,
    iree_net_rdma_device_event_subscription_t* subscription);

// Removes a subscription and joins any concurrent callback publication. Work
// already published by the callback must still join on its owning executor.
IREE_API_EXPORT void iree_net_rdma_device_events_unsubscribe(
    iree_net_rdma_device_events_t* events,
    iree_net_rdma_device_event_subscription_t* subscription);

// Consumes at most |limit| native read attempts, routing and acknowledging each
// obtained event exactly once. Returns true if the budget was exhausted before
// EAGAIN and ready-only continuation is required. Multiple poll owners may call
// concurrently. Failures are delivered to subscribers, not to the poll owner
// that happens to read the record. Event-owned native objects are never
// dereferenced after acknowledgment releases their lifetime protection.
IREE_API_EXPORT bool iree_net_rdma_device_events_poll(
    iree_net_rdma_device_events_t* events, uint32_t limit,
    uint32_t* out_completed_count);

// Records terminal failure of native event monitoring and notifies all owners.
// Takes ownership of |status|. New subscribers also receive the terminal error.
IREE_API_EXPORT void iree_net_rdma_device_events_fail(
    iree_net_rdma_device_events_t* events, iree_status_t status);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // IREE_NET_RDMA_DEVICE_EVENTS_H_
