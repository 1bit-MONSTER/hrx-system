// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef AMDF_SRC_GPU_MEMORY_RESERVATION_H_
#define AMDF_SRC_GPU_MEMORY_RESERVATION_H_

#include "libamdf/src/gpu/umd/kernel_queue.h"
#include "libamdf/src/memory_reservation.h"

#ifdef __cplusplus
extern "C" {
#endif

amdf_status_t amdf_gpu_device_query_memory_reservation_profile(
    amdf_device_t* device, uint32_t address_domain_ordinal,
    amdf_memory_reservation_profile_t* out_profile);

// Translates already validated public sources into exact native owners.
amdf_status_t amdf_gpu_memory_reservation_update_on_queue(
    amdf_memory_reservation_t* reservation, amdf_gpu_umd_kernel_queue_t* queue,
    uint32_t access_ordinal, uint32_t update_count,
    const amdf_memory_mapping_update_t* updates, uint64_t submission,
    bool* mutation_started, bool* accepted);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // AMDF_SRC_GPU_MEMORY_RESERVATION_H_
