// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef IREE_HAL_DRIVERS_AMD_FABRIC_MEMORY_H_
#define IREE_HAL_DRIVERS_AMD_FABRIC_MEMORY_H_

#include "amdf/amdf.h"
#include "iree/hal/device_event.h"

#ifdef __cplusplus
extern "C" {
#endif

// One native backing with an optional persistent host view. Borrowed scopes,
// native devices, and the API table outlive this owner and every device use.
// Addresses belong to prepared binding tables, not to the backing itself.
typedef struct iree_hal_amd_fabric_memory_t {
  // Owned native storage, consumed at deinitialization.
  amdf_memory_t* handle;
  // Owned optional host view, destroyed before the native storage.
  amdf_host_mapping_t* mapping;
  // Borrowed host pointer and logical extent; empty without a host view.
  iree_byte_span_t contents;
  // Observed cache class; UNKNOWN without a host view.
  amdf_host_cacheability_t host_cacheability;
} iree_hal_amd_fabric_memory_t;

// Qualified construction facts copied by a provider. Accesses and scope are
// borrowed; native construction establishes all accesses before publication.
typedef struct iree_hal_amd_fabric_memory_options_t {
  // Scope accepting the selected profile.
  amdf_memory_scope_t* scope;
  // Qualified native construction and mapping capabilities.
  amdf_memory_profile_t profile;
  // Number of entries in |accesses|.
  uint32_t access_count;
  // Complete caller-ordered native device access set.
  const amdf_memory_device_access_t* accesses;
  // Backing properties required during construction.
  amdf_memory_flags_t required_flags;
  // Persistent host access, or zero to omit the mapping entirely.
  amdf_memory_map_flags_t host_access;
} iree_hal_amd_fabric_memory_options_t;

// Allocates native backing, rounding the extent to the profile granularity.
// A nonzero host_access creates the requested view before publication. Failure
// leaves out_memory empty; native cleanup failures are reported to event_sink.
iree_status_t iree_hal_amd_fabric_memory_allocate(
    const amdf_api_t* api, const iree_hal_amd_fabric_memory_options_t* options,
    uint64_t byte_length, uint64_t alignment,
    iree_hal_device_event_sink_t event_sink,
    iree_hal_amd_fabric_memory_t* out_memory);

// Releases native ownership after all uses retire, without waiting for them.
// Native release failures are diagnosed; consumed handles are never retried.
void iree_hal_amd_fabric_memory_deinitialize(
    const amdf_api_t* api, iree_hal_device_event_sink_t event_sink,
    iree_hal_amd_fabric_memory_t* memory);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // IREE_HAL_DRIVERS_AMD_FABRIC_MEMORY_H_
