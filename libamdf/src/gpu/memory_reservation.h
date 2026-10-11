// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef AMDF_SRC_GPU_MEMORY_RESERVATION_H_
#define AMDF_SRC_GPU_MEMORY_RESERVATION_H_

#include "libamdf/src/memory_reservation.h"

#ifdef __cplusplus
extern "C" {
#endif

amdf_status_t amdf_gpu_device_query_memory_reservation_capabilities(
    amdf_device_t* device, uint32_t address_domain_ordinal,
    amdf_memory_reservation_capabilities_t* out_capabilities);

amdf_status_t amdf_gpu_memory_reservation_create(
    amdf_instance_t* instance,
    const amdf_memory_reservation_create_info_t* create_info,
    amdf_memory_reservation_t** out_reservation);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // AMDF_SRC_GPU_MEMORY_RESERVATION_H_
