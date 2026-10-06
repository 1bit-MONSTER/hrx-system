// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef IREE_NET_RDMA_CONTEXT_H_
#define IREE_NET_RDMA_CONTEXT_H_

#include "iree/base/api.h"
#include "iree/net/rdma/device_events.h"
#include "iree/net/rdma/library.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

// Explicit owner of an independently opened native device, one protection
// domain, and their library lifetime. Connections and registrations retain this
// owner independently; no proactor, worker or address registry is created.
typedef struct iree_net_rdma_context_t iree_net_rdma_context_t;

typedef struct iree_net_rdma_context_options_t {
  // Device name, or empty to select the first device with an active port.
  // Borrowed only for context creation.
  iree_string_view_t device_name;
  // One-based native port, or zero to select the first active port.
  uint8_t port_number;
} iree_net_rdma_context_options_t;

// Returns automatic device/active-port selection options.
static inline iree_net_rdma_context_options_t
iree_net_rdma_context_options_default(void) {
  iree_net_rdma_context_options_t options;
  memset(&options, 0, sizeof(options));
  return options;
}

// Creates a shared native owner with an independent async event stream.
// CM routes must match the underlying native device and selected port, not
// merely a device name. Their routing context need not be the opened context
// used for queues and registrations. No connection-specific PD or memory
// registration is made.
// Missing requested devices return NOT_FOUND; no active port returns
// UNAVAILABLE. Native/library/allocation failures propagate. Output is NULL
// on failure. Device and port properties are a creation-time snapshot.
IREE_API_EXPORT iree_status_t iree_net_rdma_context_create(
    iree_net_rdma_context_options_t options, iree_allocator_t host_allocator,
    iree_net_rdma_context_t** out_context);

IREE_API_EXPORT void iree_net_rdma_context_retain(
    iree_net_rdma_context_t* context);
IREE_API_EXPORT void iree_net_rdma_context_release(
    iree_net_rdma_context_t* context);

// Borrowed native interfaces. Callers must retain the context through every
// use, including destruction of native objects constructed with these handles.
// Native access and callbacks must retire before their resource owners release
// the context. Context release is not an implicit queue or GPU execution wait.
IREE_API_EXPORT const iree_net_rdma_library_t* iree_net_rdma_context_library(
    const iree_net_rdma_context_t* context);
IREE_API_EXPORT struct ibv_context* iree_net_rdma_context_device(
    const iree_net_rdma_context_t* context);
IREE_API_EXPORT struct ibv_pd* iree_net_rdma_context_protection_domain(
    const iree_net_rdma_context_t* context);
// Borrowed exclusive consumer of this context's native async event stream.
// Native users subscribe before starting queues and join before destroying CQs;
// they must not consume or acknowledge events independently of this owner.
IREE_API_EXPORT iree_net_rdma_device_events_t*
iree_net_rdma_context_device_events(const iree_net_rdma_context_t* context);
IREE_API_EXPORT uint8_t
iree_net_rdma_context_port_number(const iree_net_rdma_context_t* context);
IREE_API_EXPORT const struct ibv_device_attr*
iree_net_rdma_context_device_attributes(const iree_net_rdma_context_t* context);
IREE_API_EXPORT const struct ibv_port_attr*
iree_net_rdma_context_port_attributes(const iree_net_rdma_context_t* context);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // IREE_NET_RDMA_CONTEXT_H_
