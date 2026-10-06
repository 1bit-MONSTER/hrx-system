// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Included with IREE_NET_RDMA_SYMBOL(library, result, name, arguments).

// Creates an independently monitored asynchronous CM channel.
IREE_NET_RDMA_SYMBOL(cm, struct rdma_event_channel*, rdma_create_event_channel,
                     (void))
// Closes a channel after its IDs and monitor have retired.
IREE_NET_RDMA_SYMBOL(cm, void, rdma_destroy_event_channel,
                     (struct rdma_event_channel*))
// Obtains one owned native CM event record.
IREE_NET_RDMA_SYMBOL(cm, int, rdma_get_cm_event,
                     (struct rdma_event_channel*, struct rdma_cm_event**))
// Acknowledges and frees an obtained event before owner callback dispatch.
IREE_NET_RDMA_SYMBOL(cm, int, rdma_ack_cm_event, (struct rdma_cm_event*))
// Creates a caller-owned connection identifier on an explicit event channel.
IREE_NET_RDMA_SYMBOL(cm, int, rdma_create_id,
                     (struct rdma_event_channel*, struct rdma_cm_id**, void*,
                      enum rdma_port_space))
// Destroys an ID after its QP and delivered events have retired.
IREE_NET_RDMA_SYMBOL(cm, int, rdma_destroy_id, (struct rdma_cm_id*))
// Transfers an accepted ID to its connection's own event channel.
IREE_NET_RDMA_SYMBOL(cm, int, rdma_migrate_id,
                     (struct rdma_cm_id*, struct rdma_event_channel*))
// Binds a numeric local address during listener setup.
IREE_NET_RDMA_SYMBOL(cm, int, rdma_bind_addr,
                     (struct rdma_cm_id*, struct sockaddr*))
// Starts accepting asynchronous connection requests.
IREE_NET_RDMA_SYMBOL(cm, int, rdma_listen, (struct rdma_cm_id*, int))
// Starts native asynchronous address resolution.
IREE_NET_RDMA_SYMBOL(cm, int, rdma_resolve_addr,
                     (struct rdma_cm_id*, struct sockaddr*, struct sockaddr*,
                      int))
// Starts native asynchronous route resolution after address selection.
IREE_NET_RDMA_SYMBOL(cm, int, rdma_resolve_route, (struct rdma_cm_id*, int))
// Queries state-specific route attributes without transferring QP ownership.
IREE_NET_RDMA_SYMBOL(cm, int, rdma_init_qp_attr,
                     (struct rdma_cm_id*, struct ibv_qp_attr*, int*))
// Starts the active connection handshake.
IREE_NET_RDMA_SYMBOL(cm, int, rdma_connect,
                     (struct rdma_cm_id*, struct rdma_conn_param*))
// Completes an active handshake after configuring an independently owned QP.
IREE_NET_RDMA_SYMBOL(cm, int, rdma_establish, (struct rdma_cm_id*))
// Accepts a pending connection request.
IREE_NET_RDMA_SYMBOL(cm, int, rdma_accept,
                     (struct rdma_cm_id*, struct rdma_conn_param*))
// Rejects a pending request without creating a data connection.
IREE_NET_RDMA_SYMBOL(cm, int, rdma_reject,
                     (struct rdma_cm_id*, const void*, uint8_t))
// Initiates or replies to the native disconnect handshake.
// Independently owned QPs still require explicit retirement by their owner.
IREE_NET_RDMA_SYMBOL(cm, int, rdma_disconnect, (struct rdma_cm_id*))

// Enumerates native devices without opening their event streams.
IREE_NET_RDMA_SYMBOL(verbs, struct ibv_device**, ibv_get_device_list, (int*))
// Releases an inventory; opened contexts retain their selected device.
IREE_NET_RDMA_SYMBOL(verbs, void, ibv_free_device_list, (struct ibv_device**))
// Opens an independently owned device and async event stream.
IREE_NET_RDMA_SYMBOL(verbs, struct ibv_context*, ibv_open_device,
                     (struct ibv_device*))
// Closes the device after its native resources have retired.
IREE_NET_RDMA_SYMBOL(verbs, int, ibv_close_device, (struct ibv_context*))
// Obtains one owned native device/object event from the async stream.
IREE_NET_RDMA_SYMBOL(verbs, int, ibv_get_async_event,
                     (struct ibv_context*, struct ibv_async_event*))
// Releases native object lifetime before dispatching an owner failure.
IREE_NET_RDMA_SYMBOL(verbs, void, ibv_ack_async_event,
                     (struct ibv_async_event*))
// Returns a borrowed device name for explicit selection.
IREE_NET_RDMA_SYMBOL(verbs, const char*, ibv_get_device_name,
                     (struct ibv_device*))
// Queries creation-time device limits.
IREE_NET_RDMA_SYMBOL(verbs, int, ibv_query_device,
                     (struct ibv_context*, struct ibv_device_attr*))
// Compatibility symbol used by the extended port-query wrapper.
IREE_NET_RDMA_SYMBOL(verbs, int, ibv_query_port,
                     (struct ibv_context*, uint8_t,
                      struct _compat_ibv_port_attr*))
// Queries all valid GIDs with an explicit entry stride.
IREE_NET_RDMA_SYMBOL(verbs, ssize_t, _ibv_query_gid_table,
                     (struct ibv_context*, struct ibv_gid_entry*, size_t,
                      uint32_t, size_t))
// Creates an independently owned protection domain.
IREE_NET_RDMA_SYMBOL(verbs, struct ibv_pd*, ibv_alloc_pd, (struct ibv_context*))
// Releases a domain after its queues and registrations have retired.
IREE_NET_RDMA_SYMBOL(verbs, int, ibv_dealloc_pd, (struct ibv_pd*))

// Registration uses an explicit NIC address, independent of a CPU mapping.
IREE_NET_RDMA_SYMBOL(verbs, struct ibv_mr*, ibv_reg_mr_iova2,
                     (struct ibv_pd*, void*, size_t, uint64_t, unsigned int))
// Deregisters memory after every native access has joined.
IREE_NET_RDMA_SYMBOL(verbs, int, ibv_dereg_mr, (struct ibv_mr*))

// Creates a pollable completion-notification channel.
IREE_NET_RDMA_SYMBOL(verbs, struct ibv_comp_channel*, ibv_create_comp_channel,
                     (struct ibv_context*))
// Releases a channel after its CQs and poll monitors have retired.
IREE_NET_RDMA_SYMBOL(verbs, int, ibv_destroy_comp_channel,
                     (struct ibv_comp_channel*))
// Creates a bounded native completion queue.
IREE_NET_RDMA_SYMBOL(verbs, struct ibv_cq*, ibv_create_cq,
                     (struct ibv_context*, int, void*, struct ibv_comp_channel*,
                      int))
// Releases a CQ after its work and notifications have retired.
IREE_NET_RDMA_SYMBOL(verbs, int, ibv_destroy_cq, (struct ibv_cq*))
// Consumes a channel notification, separately from its CQ entries.
IREE_NET_RDMA_SYMBOL(verbs, int, ibv_get_cq_event,
                     (struct ibv_comp_channel*, struct ibv_cq**, void**))
// Returns ownership of consumed channel notifications.
IREE_NET_RDMA_SYMBOL(verbs, void, ibv_ack_cq_events,
                     (struct ibv_cq*, unsigned int))
// Creates a native queue pair in an explicit protection domain.
IREE_NET_RDMA_SYMBOL(verbs, struct ibv_qp*, ibv_create_qp,
                     (struct ibv_pd*, struct ibv_qp_init_attr*))
// Configures or transitions a queue pair, including terminal shutdown.
IREE_NET_RDMA_SYMBOL(verbs, int, ibv_modify_qp,
                     (struct ibv_qp*, struct ibv_qp_attr*, int))
// Destroys a queue pair and retires its native access.
IREE_NET_RDMA_SYMBOL(verbs, int, ibv_destroy_qp, (struct ibv_qp*))

#undef IREE_NET_RDMA_SYMBOL
