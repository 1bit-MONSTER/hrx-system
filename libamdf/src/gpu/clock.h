// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef AMDF_SRC_GPU_CLOCK_H_
#define AMDF_SRC_GPU_CLOCK_H_

#include "amdf/gpu.h"

#ifdef __cplusplus
extern "C" {
#endif

// Queries native clock units for one admitted queue family on a live device.
amdf_status_t AMDF_CALL amdf_gpu_device_query_queue_clock_info(
    amdf_device_t* device, uint32_t queue_family_ordinal,
    amdf_observation_clock_info_t* out_info);

// Samples a native device/host tuple for an admitted queue family.
amdf_status_t AMDF_CALL amdf_gpu_device_query_clock_sample(
    amdf_device_t* device, uint32_t queue_family_ordinal,
    amdf_observation_clock_sample_t* out_sample);

#ifdef __cplusplus
}  // extern "C"
#endif
#endif  // AMDF_SRC_GPU_CLOCK_H_
