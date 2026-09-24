// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "libamdf/src/gpu/umd/memory.h"

#include "amdf/gpu.h"

amdf_status_t amdf_gpu_umd_memory_describe_site(
    const amdf_memory_site_query_t* query,
    amdf_memory_site_description_t* out_description) {
  const amdf_queue_family_info_t* family = query->queue_family_info;
  if (family->command_type != AMDF_QUEUE_COMMAND_TYPE_GPU_PM4 ||
      family->format_version != AMDF_GPU_PM4_QUEUE_FORMAT_VERSION_1 ||
      (family->roles & AMDF_QUEUE_ROLE_CACHE_CONTROL) == 0 ||
      (family->cache_operations &
       (AMDF_CACHE_OPERATIONS_RELEASE_TO_SYSTEM |
        AMDF_CACHE_OPERATIONS_ACQUIRE_FROM_SYSTEM)) !=
          (AMDF_CACHE_OPERATIONS_RELEASE_TO_SYSTEM |
           AMDF_CACHE_OPERATIONS_ACQUIRE_FROM_SYSTEM) ||
      (family->cache_transition_kinds & AMDF_CACHE_TRANSITION_KINDS_GLOBAL) ==
          0) {
    return amdf_make_api_status(AMDF_STATUS_CODE_UNSUPPORTED);
  }
  amdf_memory_site_description_t description = {0};
  if ((query->access & AMDF_MEMORY_ACCESS_READ) != 0) {
    description.capabilities |= AMDF_MEMORY_SITE_CAPABILITY_READ;
  }
  if ((query->access & AMDF_MEMORY_ACCESS_WRITE) != 0) {
    description.capabilities |= AMDF_MEMORY_SITE_CAPABILITY_WRITE;
  }
  description.release = (amdf_cache_transition_t){
      .kind = AMDF_CACHE_TRANSITION_KIND_GLOBAL,
      .executor = AMDF_CACHE_TRANSITION_EXECUTOR_QUEUE,
      .operation = AMDF_CACHE_OPERATION_RELEASE_TO_SYSTEM,
  };
  description.acquire = (amdf_cache_transition_t){
      .kind = AMDF_CACHE_TRANSITION_KIND_GLOBAL,
      .executor = AMDF_CACHE_TRANSITION_EXECUTOR_QUEUE,
      .operation = AMDF_CACHE_OPERATION_ACQUIRE_FROM_SYSTEM,
  };
  *out_description = description;
  return AMDF_STATUS_OK;
}
