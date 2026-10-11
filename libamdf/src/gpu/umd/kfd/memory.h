// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef AMDF_SRC_GPU_UMD_KFD_MEMORY_H_
#define AMDF_SRC_GPU_UMD_KFD_MEMORY_H_

#include "libamdf/src/gpu/umd/memory.h"

#ifdef __cplusplus
extern "C" {
#endif

// Returns the eager GEM source for an established MAPPING_SOURCE consumer.
// Both memory and the exact consumer were validated by the public boundary.
uint32_t amdf_gpu_kfd_memory_mapping_handle(amdf_gpu_umd_memory_t* memory,
                                            amdf_gpu_umd_device_t* device);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // AMDF_SRC_GPU_UMD_KFD_MEMORY_H_
