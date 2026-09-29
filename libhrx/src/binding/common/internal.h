// Copyright 2025 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef IREE_EXPERIMENTAL_STREAMING_INTERNAL_H_
#define IREE_EXPERIMENTAL_STREAMING_INTERNAL_H_

#include "buffer_table.h"
#include "common/allocation_preparation.h"
#include "common/capture_admission.h"
#include "common/context.h"
#include "common/device.h"
#include "common/event.h"
#include "common/event_timestamp_pool.h"
#include "common/execution_resource.h"
#include "common/fat_binary.h"
#include "common/function_attributes.h"
#include "common/graph.h"
#include "common/hrx_bridge.h"
#include "common/memory.h"
#include "common/module.h"
#include "common/registry.h"
#include "common/stream.h"
#include "common/stream_value.h"
#include "iree/async/frontier_tracker.h"
#include "iree/base/api.h"
#include "iree/base/internal/arena.h"
#include "iree/base/threading/mutex.h"
#include "iree/base/threading/notification.h"
#include "iree/hal/api.h"
#include "iree_hal_compat.h"

#ifdef __cplusplus
extern "C" {
#endif

// mem_pool is now hrx_mem_pool_t from libhrx (no binding-internal type).
// async commit context removed (dead code, pool is now hrx_mem_pool_t).

//===----------------------------------------------------------------------===//
// Global state
//===----------------------------------------------------------------------===//

// Initializes global state.
// Synchronization: none (one-time initialization).
iree_status_t iree_hal_streaming_init_global(
    const iree_hal_device_create_params_extension_t* device_extensions,
    iree_allocator_t host_allocator);

// Cleans up global state and releases all resources.
// Synchronization: all contexts (synchronizes all active contexts).
void iree_hal_streaming_cleanup_global(void);

// Global context list management.
// Synchronization: none (thread-safe internal locking).
void iree_hal_streaming_register_context(iree_hal_streaming_context_t* context);
void iree_hal_streaming_unregister_context(
    iree_hal_streaming_context_t* context);

//===----------------------------------------------------------------------===//
// Execution control
//===----------------------------------------------------------------------===//

// Launches a host function on the stream.
// The function will be called with user_data when the stream reaches this
// point. The stream will be flushed before enqueueing the host call to ensure
// proper ordering with device operations.
// Synchronization: stream flush (flushes stream before enqueue).
iree_status_t iree_hal_streaming_launch_host_function(
    iree_hal_streaming_stream_t* stream, void (*fn)(void*), void* user_data);

//===----------------------------------------------------------------------===//
// Memory pool management
//
// Pools are now backed by hrx_mem_pool_t from libhrx. The binding stores
// hrx_mem_pool_t handles on the device and forwards HIP pool operations
// through the pyre API. The binding-internal types below are only kept for
// HIP-specific enum conversions.
//===----------------------------------------------------------------------===//

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
}
#endif

#endif  // IREE_EXPERIMENTAL_STREAMING_INTERNAL_H_
