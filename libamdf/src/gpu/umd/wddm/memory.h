// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef AMDF_SRC_GPU_UMD_WDDM_MEMORY_H_
#define AMDF_SRC_GPU_UMD_WDDM_MEMORY_H_

#include "libamdf/src/gpu/umd/memory.h"
#include "libamdf/src/platform/windows/kmt_api.h"

#ifdef __cplusplus
extern "C" {
#endif

// One native allocation span within an already prepared ordinary resource.
typedef struct amdf_gpu_wddm_memory_span_t {
  // Borrowed allocation handle in the source consumer's logical KMT device.
  D3DKMT_HANDLE allocation;
  // Source offset within this native allocation, in bytes.
  uint64_t byte_offset;
  // Available bytes from this offset to the native allocation boundary.
  uint64_t byte_length;
} amdf_gpu_wddm_memory_span_t;

// Direct indexed lookup of a validated backing-relative offset. The caller has
// checked the logical source range and added its imported prefix exactly once.
amdf_gpu_wddm_memory_span_t amdf_gpu_wddm_memory_span(
    const amdf_gpu_umd_memory_t* memory, uint64_t byte_offset);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // AMDF_SRC_GPU_UMD_WDDM_MEMORY_H_
