// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef AMDF_SRC_GPU_UMD_KFD_TARGET_COMPUTE_STORAGE_H_
#define AMDF_SRC_GPU_UMD_KFD_TARGET_COMPUTE_STORAGE_H_

#include "libamdf/src/gpu/umd/kfd/buffer.h"
#include "libamdf/src/gpu/umd/kfd/topology.h"

// Native storage shared by PM4 and AQL compute queue construction.
typedef struct amdf_gpu_kfd_compute_storage_plan_t {
  // Device-local end-of-pipe ring allocation, or zero when firmware owns it.
  amdf_gpu_kfd_buffer_create_info_t end_of_pipe_storage;
  // Host-mapped allocation: all XCC context regions followed by all debug data.
  amdf_gpu_kfd_buffer_create_info_t context_storage;
  // Per-XCC context-save/restore bytes reported to KFD.
  uint32_t context_save_restore_byte_length;
  // Per-XCC control-stack bytes reported to KFD.
  uint32_t control_stack_byte_length;
  // Number of independently addressed context-save headers.
  uint32_t context_count;
  // Debug-state byte offset from the start of the combined allocation.
  uint32_t debug_byte_offset;
  // Combined debug-state byte length for every XCC.
  uint32_t debug_byte_length;
} amdf_gpu_kfd_compute_storage_plan_t;

#ifdef __cplusplus
extern "C" {
#endif

// Resolves KFD's save-area ABI from the native geometry and reported sizes.
// Older CDNA kernels use the architectural save layout. RDNA requires the
// reported sizes because its per-CU register save area varies between ASICs.
// Unsupported or unrepresentable storage leaves the output unchanged.
bool amdf_gpu_kfd_compute_storage_plan(
    const amdf_gpu_kfd_topology_t* topology,
    amdf_queue_command_type_t command_type, size_t page_size,
    amdf_gpu_kfd_compute_storage_plan_t* out_plan);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // AMDF_SRC_GPU_UMD_KFD_TARGET_COMPUTE_STORAGE_H_
