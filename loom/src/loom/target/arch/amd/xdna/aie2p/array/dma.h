// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// AIE2P array DMA vocabulary shared by planning and native programs.

#ifndef LOOM_TARGET_ARCH_AMD_XDNA_AIE2P_ARRAY_DMA_H_
#define LOOM_TARGET_ARCH_AMD_XDNA_AIE2P_ARRAY_DMA_H_

#include "iree/base/api.h"

#ifdef __cplusplus
extern "C" {
#endif

// DMA transfer direction relative to local memory.
typedef enum loom_aie2p_array_dma_direction_e {
  LOOM_AIE2P_ARRAY_DMA_DIRECTION_MEMORY_TO_STREAM = 1,
  LOOM_AIE2P_ARRAY_DMA_DIRECTION_STREAM_TO_MEMORY = 2,
} loom_aie2p_array_dma_direction_t;

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TARGET_ARCH_AMD_XDNA_AIE2P_ARRAY_DMA_H_
