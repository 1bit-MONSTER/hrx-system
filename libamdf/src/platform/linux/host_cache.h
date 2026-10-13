// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef AMDF_SRC_PLATFORM_LINUX_HOST_CACHE_H_
#define AMDF_SRC_PLATFORM_LINUX_HOST_CACHE_H_

#include "amdf/amdf.h"

#ifdef __cplusplus
extern "C" {
#endif

// Qualifies native cache maintenance and returns its cache-line size in bytes.
amdf_status_t amdf_linux_host_cache_query_line_size(uint32_t* out_line_size);

// Writes back and invalidates every cache line intersecting the owned range.
// The caller establishes cache ownership and passes the qualified line size.
// Both publication and acquisition use this operation on noncoherent devices.
void amdf_linux_host_cache_transfer(void* pointer, uint64_t byte_length,
                                    uint32_t line_size);

// Installs each CPU PTE for a pinned, offset-correct shared exporter mapping.
// CLFLUSH faults through the exporter using read permission without modifying
// payload or leaving dirty lines. PFNMAP mappings do not support MADV_POPULATE;
// MAP_POPULATE can silently leave them absent. The caller supplies native page
// geometry and guarantees read permission, cache support and stable backing.
// Cold mapping operation, not a payload cache-ownership transition.
void amdf_linux_host_cache_prepare_translations(void* pointer,
                                                uint64_t byte_length,
                                                uint64_t page_size);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // AMDF_SRC_PLATFORM_LINUX_HOST_CACHE_H_
