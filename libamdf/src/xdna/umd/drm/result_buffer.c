// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "libamdf/src/xdna/umd/result_buffer.h"

#include <sys/ioctl.h>

#include "libamdf/src/platform/linux/file.h"
#include "libamdf/src/xdna/umd/drm/context.h"
#include "libamdf/src/xdna/umd/drm/memory.h"

amdf_status_t amdf_xdna_umd_context_query_result_buffer_info(
    amdf_xdna_umd_context_t* context,
    amdf_xdna_result_buffer_info_t* out_info) {
  const uint64_t page_size = context->device->page_size;
  *out_info = (amdf_xdna_result_buffer_info_t){
      .binding_mode = AMDF_XDNA_RESULT_BUFFER_BINDING_WHOLE_MEMORY,
      .minimum_byte_length = page_size,
      .maximum_byte_length = context->device->heap.byte_length,
      .byte_offset_alignment = page_size,
      .byte_length_granularity = page_size,
  };
  return AMDF_STATUS_OK;
}

amdf_status_t amdf_xdna_umd_context_result_buffer(
    amdf_xdna_umd_context_t* context,
    amdf_xdna_result_buffer_operation_t operation,
    amdf_xdna_umd_memory_t* memory, uint64_t byte_offset,
    uint64_t byte_length) {
  // Common admission requires a page-aligned whole allocation. The native
  // ABI takes a BO handle and always uses its full extent, without a subrange.
  (void)byte_offset;
  if (operation == AMDF_XDNA_RESULT_BUFFER_OPERATION_COLLECT) {
    const struct amdxdna_drm_sync_bo request = {
        .handle = memory->buffer.handle,
        .direction = SYNC_DIRECT_FROM_DEVICE,
        .size = byte_length,
    };
    return ioctl(context->device->descriptor, DRM_IOCTL_AMDXDNA_SYNC_BO,
                 &request) == 0
               ? AMDF_STATUS_OK
               : amdf_linux_error(errno);
  }
  const struct amdxdna_drm_config_hwctx request = {
      .handle = context->handle,
      .param_type = operation == AMDF_XDNA_RESULT_BUFFER_OPERATION_ATTACH
                        ? DRM_AMDXDNA_HWCTX_ASSIGN_DBG_BUF
                        : DRM_AMDXDNA_HWCTX_REMOVE_DBG_BUF,
      .param_val = memory->buffer.handle,
  };
  return ioctl(context->device->descriptor, DRM_IOCTL_AMDXDNA_CONFIG_HWCTX,
               &request) == 0
             ? AMDF_STATUS_OK
             : amdf_linux_error(errno);
}
