// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef AMDF_SRC_GPU_UMD_CLOCK_H_
#define AMDF_SRC_GPU_UMD_CLOCK_H_

#include "libamdf/src/gpu/umd/device.h"

#ifdef __cplusplus
extern "C" {
#endif

// Reads native rate metadata for an already admitted queue command type.
// No state is retained. Failure preserves out_info.
amdf_status_t amdf_gpu_umd_query_queue_clock_info(
    amdf_gpu_umd_device_t* device, amdf_queue_command_type_t command_type,
    amdf_observation_clock_info_t* out_info);

// Samples native device/host clocks without submission or retained state.
// Failure preserves out_sample.
amdf_status_t amdf_gpu_umd_query_clock_sample(
    amdf_gpu_umd_device_t* device, amdf_queue_command_type_t command_type,
    amdf_observation_clock_sample_t* out_sample);

#ifdef __cplusplus
}  // extern "C"
#endif
#endif  // AMDF_SRC_GPU_UMD_CLOCK_H_
