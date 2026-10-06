// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef IREE_NET_CARRIER_RDMA_CONNECTION_EVENTS_H_
#define IREE_NET_CARRIER_RDMA_CONNECTION_EVENTS_H_

#include "iree/async/proactor.h"
#include "iree/net/rdma/context.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

// Host CM channel service for reliable-connected RDMA_PS_TCP IDs. Owns only
// native event records and monitoring, never the IDs carried by those records.
// All calls are serialized with the caller's proactor poll owner.
// A full service visit retains bounded ready progress until native records
// are exhausted. Idle channels have no progress-list entry.
typedef struct iree_net_rdma_connection_events_t
    iree_net_rdma_connection_events_t;

typedef struct iree_net_rdma_connection_events_callbacks_t {
  // Receives an already-acknowledged event snapshot and copied private data,
  // borrowed until return. The owner may migrate or destroy the event's ID
  // without retaining an unacknowledged event beneath its callback. Native CM
  // status and event kind belong to the owner's connection state machine.
  void (*on_event)(void* user_data, const struct rdma_cm_event* event);
  // Receives the first terminal channel-service failure and owns its status.
  // No further event callbacks follow. The owner retires its native IDs and
  // deactivates this service outside the callback.
  void (*on_error)(void* user_data, iree_status_t status);
  // Borrowed owner, kept alive through deactivation completion.
  void* user_data;
} iree_net_rdma_connection_events_callbacks_t;

// Creates an inactive nonblocking native event channel, retaining context and
// proactor. Both callbacks and a nonzero service batch size are required.
// Failure leaves output NULL. No native IDs, monitors or workers are created;
// native ID setup can fail and unwind synchronously before activation.
IREE_API_EXPORT iree_status_t iree_net_rdma_connection_events_create(
    iree_net_rdma_context_t* context, iree_async_proactor_t* proactor,
    uint32_t service_batch_size,
    iree_net_rdma_connection_events_callbacks_t callbacks,
    iree_allocator_t host_allocator,
    iree_net_rdma_connection_events_t** out_events);

// Begins bounded dispatch on the caller-owned proactor after native ID setup.
// Records already queued on the channel are delivered by subsequent polling.
// Failure leaves the service inactive and immediately destroyable. A successful
// activation requires deactivation before destruction; retired services cannot
// be reactivated. No callback runs inline.
IREE_API_EXPORT iree_status_t iree_net_rdma_connection_events_activate(
    iree_net_rdma_connection_events_t* events);

// Borrows the channel for explicit ID creation/migration. Only RDMA_PS_TCP IDs
// may use this service; their ownership stays with the caller.
IREE_API_EXPORT struct rdma_event_channel*
iree_net_rdma_connection_events_handle(
    iree_net_rdma_connection_events_t* events);

// Stops event callback admission and joins monitoring. Call exactly once from
// the poll owner, outside this service's event/progress callbacks. Completion
// may run inline and destroy the service. The caller keeps its poll owner
// alive through that completion, as required by event-source unregistration.
// An inactive service completes inline without submitting any work.
IREE_API_EXPORT void iree_net_rdma_connection_events_deactivate(
    iree_net_rdma_connection_events_t* events,
    iree_async_event_source_unregistered_callback_t callback);

// Destroys an inactive or deactivated service after all IDs have been destroyed
// or migrated away. Native ID destruction disposes unread events and
// undelivered listener requests; every obtained event was already acknowledged.
IREE_API_EXPORT void iree_net_rdma_connection_events_destroy(
    iree_net_rdma_connection_events_t* events);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // IREE_NET_CARRIER_RDMA_CONNECTION_EVENTS_H_
