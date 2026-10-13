// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef AMDF_SRC_HOST_MEMORY_H_
#define AMDF_SRC_HOST_MEMORY_H_

#include "libamdf/src/memory_profile.h"

#ifdef __cplusplus
extern "C" {
#endif

// Prepares CPU-only system backing in the existing resource's native slot.
// Borrowed pages remain caller-owned; allocation owns only the host VM range.
amdf_status_t amdf_host_memory_prepare(
    amdf_memory_t* memory, const amdf_memory_native_profile_t* profile,
    const amdf_memory_create_info_t* create_info, amdf_memory_info_t* out_info);

// Creates a lightweight WRITE_BACK view borrowing a persistent host range.
// memory is NULL for a reservation-owned range. Cache operations need only
// the retained address, geometry and direct CPU recipe, never source lookup.
amdf_status_t amdf_host_memory_create_view(
    amdf_allocator_t host_allocator, amdf_memory_t* memory, void* base_pointer,
    const amdf_memory_map_info_t* map_info, amdf_host_mapping_t** out_mapping);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // AMDF_SRC_HOST_MEMORY_H_
