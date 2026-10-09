// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef IREE_HAL_DRIVERS_AMDGPU_ALLOCATOR_H_
#define IREE_HAL_DRIVERS_AMDGPU_ALLOCATOR_H_

#include "iree/base/api.h"
#include "iree/hal/api.h"
#include "iree/hal/drivers/amdgpu/util/libhsa.h"

typedef struct iree_hal_amdgpu_logical_device_t
    iree_hal_amdgpu_logical_device_t;
typedef struct iree_hal_amdgpu_topology_t iree_hal_amdgpu_topology_t;

//===----------------------------------------------------------------------===//
// iree_hal_amdgpu_allocator_t
//===----------------------------------------------------------------------===//

// Creates a buffer allocator that allocates from HSA memory pools.
//
// Normal allocations are direct HSA memory-pool allocations. When the logical
// device has HAL ASAN enabled, device-local persistent allocations route
// through the device default pool set so redzones, shadow publication, release
// poisoning, and quarantine follow the same policy as queue allocations.
//
// |logical_device| is unretained and must outlive the allocator.
iree_status_t iree_hal_amdgpu_allocator_create(
    iree_hal_amdgpu_logical_device_t* logical_device,
    const iree_hal_amdgpu_libhsa_t* libhsa,
    const iree_hal_amdgpu_topology_t* topology, iree_allocator_t host_allocator,
    iree_hal_allocator_t** out_allocator);

// Returns the native GPU-local pool and allocation flags that implement
// uncached device access for |physical_device_ordinal|. Returns false when the
// native runtime exposes no such route. The returned pool is borrowed from the
// allocator and remains valid for the allocator's lifetime.
bool iree_hal_amdgpu_allocator_query_uncached_memory_pool(
    iree_hal_allocator_t* allocator, uint32_t physical_device_ordinal,
    hsa_amd_memory_pool_t* out_memory_pool, uint32_t* out_allocation_flags);

// Attaches typed external storage as one complete AMDGPU root buffer.
//
// The returned buffer owns only the native ROCr attachment and never consumes
// or releases |storage|'s transport owner. |storage| must remain alive until
// the returned buffer is released. Explicit pool providers use this primitive
// to preserve their separate transport and native attachment lifetimes.
// |access_agents| is the exact group-qualified native access set and is copied
// by the synchronous attachment calls.
iree_status_t iree_hal_amdgpu_allocator_import_direct_buffer(
    iree_hal_allocator_t* allocator, const iree_hal_buffer_params_t* params,
    const iree_hal_external_storage_t* storage,
    iree_host_size_t access_agent_count, const hsa_agent_t* access_agents,
    iree_hal_buffer_t** out_buffer);

#endif  // IREE_HAL_DRIVERS_AMDGPU_ALLOCATOR_H_
