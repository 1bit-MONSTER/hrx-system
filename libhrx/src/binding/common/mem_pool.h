// Copyright 2025 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LIBHRX_SRC_BINDING_COMMON_MEM_POOL_H_
#define LIBHRX_SRC_BINDING_COMMON_MEM_POOL_H_

#include "hrx_runtime.h"
#include "iree/base/api.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct iree_hal_streaming_device_t iree_hal_streaming_device_t;

// Memory access flags for memory pools (for HIP API conversion).
typedef enum iree_hal_streaming_mem_access_flag_bits_e {
  IREE_HAL_STREAMING_MEM_ACCESS_FLAG_PROT_NONE = 0ull,
  IREE_HAL_STREAMING_MEM_ACCESS_FLAG_PROT_READ = 1ull << 0,
  IREE_HAL_STREAMING_MEM_ACCESS_FLAG_PROT_READWRITE =
      (1ull << 1) | IREE_HAL_STREAMING_MEM_ACCESS_FLAG_PROT_READ,
} iree_hal_streaming_mem_access_flags_t;

// Memory pool location types (for HIP API conversion).
typedef enum iree_hal_streaming_mem_location_type_e {
  IREE_HAL_STREAMING_MEM_LOCATION_TYPE_INVALID = 0,
  IREE_HAL_STREAMING_MEM_LOCATION_TYPE_DEVICE,
  IREE_HAL_STREAMING_MEM_LOCATION_TYPE_HOST,
  IREE_HAL_STREAMING_MEM_LOCATION_TYPE_HOST_NUMA,
  IREE_HAL_STREAMING_MEM_LOCATION_TYPE_HOST_NUMA_CURRENT,
} iree_hal_streaming_mem_location_type_t;

// Device pool accessors.
// Returns a device-owned pool handle that remains valid while selected.
hrx_mem_pool_t iree_hal_streaming_device_default_mem_pool(
    iree_hal_streaming_device_t* device);
// Returns a device-owned pool handle that remains valid while selected.
hrx_mem_pool_t iree_hal_streaming_device_mem_pool(
    iree_hal_streaming_device_t* device);
// Retains the selected pool for use outside the device lock. The caller must
// release the returned handle with hrx_mem_pool_release.
hrx_mem_pool_t iree_hal_streaming_device_retain_mem_pool(
    iree_hal_streaming_device_t* device);
// Retains the device default pool for use outside the device lock. The caller
// must release the returned handle with hrx_mem_pool_release.
hrx_mem_pool_t iree_hal_streaming_device_retain_default_mem_pool(
    iree_hal_streaming_device_t* device);
iree_status_t iree_hal_streaming_device_ensure_default_mem_pool(
    iree_hal_streaming_device_t* device);
// Replaces the selected pool while preserving any in-flight pool users.
void iree_hal_streaming_device_set_mem_pool(iree_hal_streaming_device_t* device,
                                            hrx_mem_pool_t pool);
// Restores the default pool only when |pool| is the selected pool.
void iree_hal_streaming_device_reset_mem_pool_if_current(
    iree_hal_streaming_device_t* device, hrx_mem_pool_t pool);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LIBHRX_SRC_BINDING_COMMON_MEM_POOL_H_
