// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/tools/loom-serve/options.h"

#include <string.h>

#include "iree/base/tooling/flags.h"
#include "iree/tooling/device_util.h"

IREE_FLAG(int32_t, workers, 8,
          "Shared CPU workers for compilation, scheduling and media jobs.");
IREE_FLAG(string, pool_backing, "elastic",
          "elastic demand-commits parameter/state slabs; fixed backs storage "
          "eagerly and supports device address sanitization.");
IREE_FLAG(
    int64_t, slab_bytes, 67108864,
    "Physical commitment/reclamation granularity for elastic parameter "
    "and state storage (default 64 MiB). Larger slabs reduce allocation "
    "and mapping operations but retain more backing for partially live "
    "slabs; logical tensor and KV block sizes are unchanged. Zero selects "
    "the allocator recommendation. Ignored for fixed backing.");
IREE_FLAG(int64_t, memory_bytes, 0,
          "Shared physical parameter/state budget; a nonzero limit requires "
          "one device. Zero imposes no serving limit. Invocation workspace "
          "is accounted separately.");

iree_status_t loom_serve_host_create_from_flags(iree_allocator_t host_allocator,
                                                loom_serve_host_t** out_host) {
  return loom_serve_host_create((iree_host_size_t)FLAG_workers, host_allocator,
                                out_host);
}

iree_status_t loom_serve_device_group_allocate_from_flags(
    loom_serve_host_t* host, iree_allocator_t host_allocator,
    loom_serve_device_group_t** out_group) {
  *out_group = NULL;
  const iree_string_view_list_t devices = iree_hal_device_flag_list();
  if (!devices.count) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "--device is required; specify a HAL device URI");
  }
  if (devices.count > IREE_HAL_TOPOLOGY_MAX_DEVICE_COUNT) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "at most %d --device values are supported",
                            IREE_HAL_TOPOLOGY_MAX_DEVICE_COUNT);
  }
  for (iree_host_size_t i = 0; i < devices.count; ++i) {
    if (!devices.values[i].size) {
      return iree_make_status(
          IREE_STATUS_INVALID_ARGUMENT,
          "--device is required; device %zu has an empty URI", i);
    }
  }
  if ((strcmp(FLAG_pool_backing, "elastic") &&
       strcmp(FLAG_pool_backing, "fixed")) ||
      FLAG_slab_bytes < 0 || FLAG_memory_bytes < 0 ||
      (FLAG_memory_bytes && !strcmp(FLAG_pool_backing, "fixed"))) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "pool_backing must be elastic or fixed; slab_bytes "
                            "and memory_bytes must be nonnegative; a physical "
                            "budget requires elastic backing");
  }
  if (devices.count > 1 && FLAG_memory_bytes) {
    return iree_make_status(
        IREE_STATUS_INVALID_ARGUMENT,
        "--memory_bytes requires one device; independent device pools cannot "
        "enforce a shared byte limit");
  }
  loom_serve_device_options_t options[IREE_HAL_TOPOLOGY_MAX_DEVICE_COUNT];
  for (iree_host_size_t i = 0; i < devices.count; ++i) {
    options[i] = (loom_serve_device_options_t){
        .uri = devices.values[i],
        .backing = !strcmp(FLAG_pool_backing, "elastic")
                       ? LOOM_SERVE_DEVICE_BACKING_ELASTIC
                       : LOOM_SERVE_DEVICE_BACKING_FIXED,
        .slab_size = (iree_device_size_t)FLAG_slab_bytes,
        .memory_limit = (uint64_t)FLAG_memory_bytes,
    };
  }
  return loom_serve_device_group_allocate(host, devices.count, options,
                                          host_allocator, out_group);
}
