// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef AMDF_SRC_XDNA_CLOCK_H_
#define AMDF_SRC_XDNA_CLOCK_H_

#include "amdf/xdna.h"

#ifdef __cplusplus
extern "C" {
#endif

// Samples native operating-rate metadata for firmware RECORD_TIMER records.
amdf_status_t AMDF_CALL amdf_xdna_device_query_timer_clock_info(
    amdf_device_t* device, amdf_observation_clock_info_t* out_info);

#ifdef __cplusplus
}  // extern "C"
#endif
#endif  // AMDF_SRC_XDNA_CLOCK_H_
