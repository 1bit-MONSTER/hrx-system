// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef IREE_NET_CARRIER_RDMA_DEVICE_FAILURE_H_
#define IREE_NET_CARRIER_RDMA_DEVICE_FAILURE_H_

#include "iree/async/proactor.h"
#include "iree/net/rdma/context.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

// Host error monitor for a CQ and its attached QPs. Native events may be read
// on another connection's proactor; the first terminal error is handed to this
// owner's proactor before invoking its callback. No successful transfers pass
// through this service. All calls are serialized with the owning poll thread.
typedef struct iree_net_rdma_device_failure_t iree_net_rdma_device_failure_t;

// Creates and arms monitoring before starting native queues. The callback owns
// its status and must schedule retirement outside this service's callbacks.
// Context/proactor are retained; the CQ and callback owner are borrowed through
// deactivation. Failure leaves output NULL without live monitoring.
IREE_API_EXPORT iree_status_t iree_net_rdma_device_failure_create(
    iree_net_rdma_context_t* context, iree_async_proactor_t* proactor,
    struct ibv_cq* queue, uint32_t service_batch_size,
    void (*on_failure)(void* user_data, iree_status_t status), void* user_data,
    iree_allocator_t host_allocator,
    iree_net_rdma_device_failure_t** out_failure);

// Stops admission and joins the native monitor and any published error handoff.
// Call once outside this service's callbacks. Previously published errors may
// still invoke the owner callback before deactivation completes. Completion may
// run inline and may destroy the service. This does not establish QP
// quiescence.
IREE_API_EXPORT void iree_net_rdma_device_failure_deactivate(
    iree_net_rdma_device_failure_t* failure,
    iree_async_event_source_unregistered_callback_t callback);

// Releases a deactivated service. Does not destroy the borrowed native CQ.
IREE_API_EXPORT void iree_net_rdma_device_failure_destroy(
    iree_net_rdma_device_failure_t* failure);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // IREE_NET_CARRIER_RDMA_DEVICE_FAILURE_H_
