// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef AMDF_SRC_GPU_UMD_KFD_HDP_H_
#define AMDF_SRC_GPU_UMD_KFD_HDP_H_

#include "libamdf/src/gpu/umd/kfd/device.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

// Returns whether the exact native target family has a qualified HDP
// host-write publication sequence. Native remap availability remains a
// separate runtime fact established by construction.
bool amdf_gpu_kfd_hdp_supports_host_publication(
    const amdf_gpu_kfd_topology_t* topology);

// Acquires this device's optional KFD HDP register mapping. Success publishes
// NULL when the target does not use the sequence, the local heap is not fully
// host-visible, the native page is too large, or KFD reports that no remap is
// available. Every other native failure is returned. The device is borrowed
// through destruction.
amdf_status_t amdf_gpu_kfd_hdp_create(amdf_gpu_umd_device_t* device,
                                      amdf_gpu_kfd_hdp_t** out_hdp);

// Publishes host writes which have already been ordered into the device's HDP
// aperture. The live mapping and qualified target family are construction
// invariants. GFX11 uses a native timestamp query for a safe posting read and
// propagates its failures; other qualified families use direct MMIO only.
amdf_status_t amdf_gpu_kfd_hdp_publish_host_writes(amdf_gpu_kfd_hdp_t* hdp);

// Releases the GPU mapping, CPU mapping, and special KFD allocation. The owner
// is consumed on every result. A failed native release leaves the remaining
// resource leaked rather than risking an ambiguous retry.
amdf_status_t amdf_gpu_kfd_hdp_destroy(amdf_gpu_kfd_hdp_t* hdp);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // AMDF_SRC_GPU_UMD_KFD_HDP_H_
