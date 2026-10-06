// Copyright 2026 The HRX Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LIBHRX_SRC_LIBHRX_DEVICE_H_
#define LIBHRX_SRC_LIBHRX_DEVICE_H_

#include "hrx_runtime.h"
#include "iree/hal/api.h"

#ifdef __cplusplus
extern "C" {
#endif

// Returns the borrowed HAL device underlying |device|, or NULL when |device|
// is NULL.
iree_hal_device_t* hrx_device_hal(hrx_device_t device);

// Queries immutable total device memory from the HAL device spec. Returns OK
// with |out_known| false when the spec does not describe a known capacity.
hrx_status_t hrx_device_query_total_memory_from_spec(
    hrx_device_t device, bool* out_known, iree_device_size_t* out_total);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LIBHRX_SRC_LIBHRX_DEVICE_H_
