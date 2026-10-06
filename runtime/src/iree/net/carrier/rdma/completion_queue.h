// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef IREE_NET_CARRIER_RDMA_COMPLETION_QUEUE_H_
#define IREE_NET_CARRIER_RDMA_COMPLETION_QUEUE_H_

#include "iree/async/proactor.h"
#include "iree/net/rdma/context.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

// Host-owned CQ and native notification service. All calls are serialized with
// the proactor poll owner. Native device-side users do not need this service.
typedef struct iree_net_rdma_completion_queue_t
    iree_net_rdma_completion_queue_t;

// Local host completion policy; independent of peer configuration and ordering.
enum iree_net_rdma_completion_queue_mode_e {
  // Sleep on native readiness when idle, continuing bounded batches while busy.
  IREE_NET_RDMA_COMPLETION_QUEUE_MODE_READINESS = 0,
  // Poll a bounded batch on every proactor turn, even when idle. Keeps the
  // caller-owned poll thread runnable while any such CQ is active; intended for
  // dedicated cores. CM and native device failures still use readiness service.
  IREE_NET_RDMA_COMPLETION_QUEUE_MODE_BUSY_POLL = 1,
};
typedef uint32_t iree_net_rdma_completion_queue_mode_t;

typedef struct iree_net_rdma_completion_queue_options_t {
  // Native capacity covering every enforced outstanding SQ and RQ request,
  // including a full error/flush burst, not merely signaled successful work.
  uint32_t capacity;
  // Maximum CQEs and notification records consumed per service visit.
  uint32_t service_batch_size;
  // Native interrupt vector selected during connection setup.
  uint32_t completion_vector;
  // Immutable host progress policy. Zero selects readiness-driven service.
  iree_net_rdma_completion_queue_mode_t mode;
} iree_net_rdma_completion_queue_options_t;

typedef struct iree_net_rdma_completion_queue_callbacks_t {
  // Delivers native entries unchanged, including failures. The owner resolves
  // WR identities and retires corresponding resources. Entries are borrowed
  // only until return; callback order does not establish cross-QP ordering.
  void (*on_completions)(void* user_data, iree_host_size_t count,
                         const struct ibv_wc* completions);
  // Receives the first terminal service error and owns its status. The owner
  // must retire native queues and deactivate this service outside callbacks.
  // No further completion callbacks are delivered after service failure.
  void (*on_error)(void* user_data, iree_status_t status);
  // Borrowed callback owner, kept alive through deactivation completion.
  void* user_data;
} iree_net_rdma_completion_queue_callbacks_t;

// Creates a host completion service before attaching any QPs. Retains
// context and proactor. Allocation/native failures leave output NULL without
// live monitoring. Both callbacks are required. The proactor may be polling
// only if this is called from its poll owner.
IREE_API_EXPORT iree_status_t iree_net_rdma_completion_queue_create(
    iree_net_rdma_context_t* context, iree_async_proactor_t* proactor,
    iree_net_rdma_completion_queue_options_t options,
    iree_net_rdma_completion_queue_callbacks_t callbacks,
    iree_allocator_t host_allocator,
    iree_net_rdma_completion_queue_t** out_queue);

// Returns the borrowed native CQ for constructing QPs. The caller enforces
// the aggregate outstanding-work bound used to size this CQ.
IREE_API_EXPORT struct ibv_cq* iree_net_rdma_completion_queue_handle(
    iree_net_rdma_completion_queue_t* queue);

// Stops callback admission and joins native monitoring. Call exactly once,
// outside this service's event/completion/error callbacks. Any owner work that
// still needs CQEs must retire first; deactivation itself does not retire QPs.
// The callback may run inline and may destroy the deactivated service. As with
// native event-source retirement, the caller keeps the poll owner alive.
IREE_API_EXPORT void iree_net_rdma_completion_queue_deactivate(
    iree_net_rdma_completion_queue_t* queue,
    iree_async_event_source_unregistered_callback_t callback);

// Destroys a deactivated service after all attached QPs have been destroyed.
// Consumed notifications are acknowledged by service visits; native destruction
// discards unread notifications without requiring a working event channel.
IREE_API_EXPORT void iree_net_rdma_completion_queue_destroy(
    iree_net_rdma_completion_queue_t* queue);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // IREE_NET_CARRIER_RDMA_COMPLETION_QUEUE_H_
