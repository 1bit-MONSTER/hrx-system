// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef AMDF_SRC_XDNA_UMD_MCDM_MEMORY_H_
#define AMDF_SRC_XDNA_UMD_MCDM_MEMORY_H_

#include "libamdf/src/xdna/umd/mcdm/device.h"
#include "libamdf/src/xdna/umd/mcdm/private_allocation.h"
#include "libamdf/src/xdna/umd/memory.h"

#ifdef __cplusplus
extern "C" {
#endif

// Registers a page-aligned borrowed host interval as one resource-less standard
// allocation. The validated length fits the native UINT32_MAX limit. Successful
// acquisition immediately belongs to the live allocation slot, including when
// a subsequent native-result check fails. The caller owns terminal cleanup.
amdf_status_t amdf_windows_xdna_memory_register_pages(
    amdf_xdna_umd_device_t* device, void* pointer, uint64_t byte_length,
    D3DKMT_HANDLE* allocation);

// Borrows the stable CPU address of an admitted ordinary mapping source.
// The offset is native-backing-relative, already checked by common admission.
// No mapping, reference, allocation, lookup or cache operation occurs here.
void* amdf_windows_xdna_memory_mapping_source(amdf_xdna_umd_memory_t* memory,
                                              uint64_t byte_offset);

// Borrows the allocation from already-admitted context-private memory. Logical
// byte zero follows the context's reserved bootstrap prefix in this allocation.
const amdf_windows_xdna_private_allocation_t*
amdf_windows_xdna_memory_private_allocation(amdf_xdna_umd_memory_t* memory);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // AMDF_SRC_XDNA_UMD_MCDM_MEMORY_H_
