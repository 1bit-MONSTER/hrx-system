// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef IREE_HAL_DRIVERS_AMDGPU_MEMORY_BACKEND_H_
#define IREE_HAL_DRIVERS_AMDGPU_MEMORY_BACKEND_H_

#include "iree/hal/drivers/amdgpu/util/libhsa.h"
#include "iree/hal/memory_backend.h"

typedef struct iree_hal_amdgpu_topology_t iree_hal_amdgpu_topology_t;
typedef struct iree_hal_amdgpu_physical_device_t
    iree_hal_amdgpu_physical_device_t;
typedef struct iree_hal_amdgpu_asan_state_t iree_hal_amdgpu_asan_state_t;

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

// Borrowed native resources established by device creation and group sealing.
// Only cold factories inspect these owners; created pools capture their used
// services and access facts without retaining either topology or this view.
typedef struct iree_hal_amdgpu_memory_backend_t {
  // Generic ROCr owner schema and shared construction factory.
  iree_hal_memory_backend_t base;
  // Existing HAL owner for native buffer placement and profiling.
  iree_hal_device_t* device;
  // Device-owned allocator borrowed as the legacy ROCr attachment service.
  iree_hal_allocator_t* allocator;
  // Device-owned native runtime dispatch table.
  const iree_hal_amdgpu_libhsa_t* libhsa;
  // Device-owned native agent snapshot, borrowed only during qualification.
  const iree_hal_amdgpu_topology_t* topology;
  // Existing per-family native memory, wrapper and progress owners.
  iree_hal_amdgpu_physical_device_t* const* physical_devices;
  // Device-owned sanitizer namespace and advice services.
  iree_hal_amdgpu_asan_state_t* asan_state;
  // Existing completion probe for the sealed group's shared tracker.
  iree_hal_pool_epoch_query_t epoch_query;
} iree_hal_amdgpu_memory_backend_t;

// Publishes the factory after the caller supplies existing native services.
// Creates no payload, native object, execution queue or allocation policy.
void iree_hal_amdgpu_memory_backend_initialize(
    iree_hal_amdgpu_memory_backend_t* backend);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // IREE_HAL_DRIVERS_AMDGPU_MEMORY_BACKEND_H_
