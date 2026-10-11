// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef IREE_HAL_DRIVERS_AMD_FABRIC_FABRIC_H_
#define IREE_HAL_DRIVERS_AMD_FABRIC_FABRIC_H_

#include "amdf/amdf.h"
#include "iree/base/api.h"

#ifdef __cplusplus
extern "C" {
#endif

// Shared native identity and passive discovery for AMD execution engines.
// The driver and every materialized device retain this owner independently.
// Fields are immutable after construction; engine resources and private scopes
// belong to the consuming device, not this object. This is not a device group:
// sharing native identity does not grant access across sealed HAL groups.
typedef struct iree_hal_amd_fabric_t {
  // References held by native owners; never modified directly by consumers.
  iree_atomic_ref_count_t ref_count;
  // Allocator owning this object and its discovery snapshot.
  iree_allocator_t host_allocator;
  // Borrowed negotiated core table, live for the complete fabric lifetime.
  const amdf_api_t* api;
  // Owned instance containing shared native provider connections.
  amdf_instance_t* instance;
  // Borrowed ordinary system storage scope, owned by the instance.
  amdf_memory_scope_t* system_scope;
  // Number of passive identities in the immutable discovery snapshot.
  uint32_t endpoint_count;
  // Owned snapshot including all native engine kinds, without activation.
  amdf_endpoint_summary_t* endpoints;
} iree_hal_amd_fabric_t;

// Creates an independent native instance and snapshots passive discovery.
// Engine extension negotiation and activation happen in engine constructors.
// The providing library and |api| must outlive the returned fabric. Failure
// publishes no output. The caller releases the returned reference.
iree_status_t iree_hal_amd_fabric_create(const amdf_api_t* api,
                                         iree_allocator_t host_allocator,
                                         iree_hal_amd_fabric_t** out_fabric);

// Retains the shared owner. NULL is ignored.
void iree_hal_amd_fabric_retain(iree_hal_amd_fabric_t* fabric);

// Releases the shared owner after all native children are gone. A native
// teardown failure is diagnosed and preserves remaining ownership. NULL is
// ignored. This performs no implicit device wait.
void iree_hal_amd_fabric_release(iree_hal_amd_fabric_t* fabric);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // IREE_HAL_DRIVERS_AMD_FABRIC_FABRIC_H_
