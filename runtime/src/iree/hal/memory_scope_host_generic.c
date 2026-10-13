// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/hal/memory_scope.h"

IREE_API_EXPORT void iree_hal_memory_transition_execute_host_direct(
    const iree_hal_memory_transition_recipe_info_t* info, const void* pointer,
    iree_device_size_t length) {
  (void)pointer;
  if (!length) {
    return;
  }
  // Backends on hosts without a direct instruction implementation must qualify
  // native maintenance through HOST_API. An empty direct program is portable.
  IREE_ASSERT(info->host.instruction == IREE_HAL_HOST_CACHE_INSTRUCTION_NONE &&
                  info->host.fence_before == IREE_HAL_HOST_CACHE_FENCE_NONE &&
                  info->host.fence_after == IREE_HAL_HOST_CACHE_FENCE_NONE,
              "backend published an unqualified direct host recipe");
}
