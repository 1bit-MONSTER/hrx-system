// Copyright 2026 The HRX Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LIBHRX_SRC_LIBHRX_RUNTIME_H_
#define LIBHRX_SRC_LIBHRX_RUNTIME_H_

#include "hrx_runtime.h"
#include "iree/hal/api.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct iree_async_proactor_pool_t iree_async_proactor_pool_t;

// Initializes GPU devices with one immutable HAL device-creation extension
// chain. The chain and all transitively referenced provider data must remain
// valid until hrx_gpu_shutdown().
hrx_status_t hrx_gpu_initialize_with_device_extensions(
    uint32_t flags,
    const iree_hal_device_create_params_extension_t* device_extensions);

// Returns the process-wide borrowed proactor pool, or NULL before runtime
// initialization and after shutdown.
iree_async_proactor_pool_t* hrx_runtime_proactor_pool(void);

// Returns the configured application event sink translated to the HAL event
// ABI. Returns false when HRX would otherwise discard device events.
bool hrx_runtime_try_get_hal_device_event_sink(
    iree_hal_device_event_sink_t* out_sink);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LIBHRX_SRC_LIBHRX_RUNTIME_H_
