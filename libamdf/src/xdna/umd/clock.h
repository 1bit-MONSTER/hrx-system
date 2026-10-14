// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef AMDF_SRC_XDNA_UMD_CLOCK_H_
#define AMDF_SRC_XDNA_UMD_CLOCK_H_

#include "libamdf/src/xdna/umd/device.h"

#ifdef __cplusplus
extern "C" {
#endif

// Queries the current native array operating rate without retaining state or
// changing power policy. Failure leaves out_info unchanged.
amdf_status_t amdf_xdna_umd_query_timer_clock_info(
    amdf_xdna_umd_device_t* device, amdf_observation_clock_info_t* out_info);

#ifdef __cplusplus
}  // extern "C"
#endif
#endif  // AMDF_SRC_XDNA_UMD_CLOCK_H_
