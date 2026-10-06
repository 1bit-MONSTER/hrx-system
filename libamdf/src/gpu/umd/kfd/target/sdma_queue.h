// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef AMDF_SRC_GPU_UMD_KFD_TARGET_SDMA_QUEUE_H_
#define AMDF_SRC_GPU_UMD_KFD_TARGET_SDMA_QUEUE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "libamdf/src/gpu/umd/kfd/target/queue_plan.h"
#include "libamdf/src/gpu/umd/kfd/topology.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

// Resolves the exact native SDMA IP and builds its KFD storage/encoding plan.
// Unsupported engine or storage facts leave `out_plan` unchanged.
bool amdf_gpu_kfd_sdma_queue_plan(const amdf_gpu_kfd_topology_t* topology,
                                  size_t page_size, uint32_t cache_line_size,
                                  amdf_gpu_kfd_user_queue_plan_t* out_plan);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // AMDF_SRC_GPU_UMD_KFD_TARGET_SDMA_QUEUE_H_
