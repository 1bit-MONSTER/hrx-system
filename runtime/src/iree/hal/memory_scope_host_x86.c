// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <immintrin.h>

#include "iree/hal/memory_scope.h"

static void iree_hal_memory_transition_host_fence(
    iree_hal_host_cache_fence_t fence) {
  switch (fence) {
    case IREE_HAL_HOST_CACHE_FENCE_NONE:
      break;
    case IREE_HAL_HOST_CACHE_FENCE_X86_SFENCE:
      _mm_sfence();
      break;
    case IREE_HAL_HOST_CACHE_FENCE_X86_MFENCE:
      _mm_mfence();
      break;
    default:
      IREE_ASSERT_UNREACHABLE("unqualified host cache fence");
      break;
  }
}

IREE_API_EXPORT void iree_hal_memory_transition_execute_host_direct(
    const iree_hal_memory_transition_recipe_info_t* info, const void* pointer,
    iree_device_size_t length) {
  if (!length) {
    return;
  }
  iree_hal_memory_transition_host_fence(info->host.fence_before);
  if (info->host.instruction == IREE_HAL_HOST_CACHE_INSTRUCTION_NONE) {
    iree_hal_memory_transition_host_fence(info->host.fence_after);
    return;
  }
  // The native producer qualifies the instruction for the host and supplies
  // its power-of-two cache-line granularity. The caller validated the inclusive
  // last address; rounding down avoids an end-address overflow.
  const uintptr_t mask = (uintptr_t)info->range_granularity - 1;
  const uintptr_t first = (uintptr_t)pointer & ~mask;
  const uintptr_t last = ((uintptr_t)pointer + length - 1) & ~mask;
  for (uintptr_t line = first;; line += info->range_granularity) {
    switch (info->host.instruction) {
      case IREE_HAL_HOST_CACHE_INSTRUCTION_X86_CLFLUSH:
        _mm_clflush((const void*)line);
        break;
      case IREE_HAL_HOST_CACHE_INSTRUCTION_X86_CLFLUSHOPT:
#if defined(IREE_COMPILER_MSVC)
        _mm_clflushopt((void*)line);
#else
        __asm__ volatile("clflushopt (%0)" : : "r"(line) : "memory");
#endif  // IREE_COMPILER_MSVC
        break;
      case IREE_HAL_HOST_CACHE_INSTRUCTION_X86_CLWB:
#if defined(IREE_COMPILER_MSVC)
        _mm_clwb((void*)line);
#else
        __asm__ volatile("clwb (%0)" : : "r"(line) : "memory");
#endif  // IREE_COMPILER_MSVC
        break;
      default:
        IREE_ASSERT_UNREACHABLE("unqualified host cache instruction");
        break;
    }
    if (line == last) {
      break;
    }
  }
  iree_hal_memory_transition_host_fence(info->host.fence_after);
}
