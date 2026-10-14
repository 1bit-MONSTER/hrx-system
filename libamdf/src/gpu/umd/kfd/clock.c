// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "libamdf/src/gpu/umd/clock.h"

#include <errno.h>
#include <sys/ioctl.h>

#include "drm/amdgpu_drm.h"
#include "libamdf/src/gpu/umd/kfd/device.h"
#include "libamdf/src/platform/linux/file.h"

amdf_status_t amdf_gpu_umd_query_queue_clock_info(
    amdf_gpu_umd_device_t* device, amdf_queue_command_type_t command_type,
    amdf_observation_clock_info_t* out_info) {
  (void)command_type;
  struct drm_amdgpu_info_device info = {0};
  struct drm_amdgpu_info query = {
      .return_pointer = (uintptr_t)&info,
      .return_size = sizeof(info),
      .query = AMDGPU_INFO_DEV_INFO,
  };
  if (ioctl(device->render_descriptor, DRM_IOCTL_AMDGPU_INFO, &query) != 0) {
    return amdf_linux_error(errno);
  }
  if (info.gpu_counter_freq == 0) {
    return amdf_make_api_status(AMDF_STATUS_CODE_UNSUPPORTED);
  }
  // DEV_INFO reports the GPU reference rate in kHz, including the native
  // platform's nominal crystal correction. Its timestamp-query epoch is not
  // assumed to be the epoch of samples written by a command processor.
  *out_info = (amdf_observation_clock_info_t){
      .frequency_hz = (uint64_t)info.gpu_counter_freq * 1000,
  };
  return AMDF_STATUS_OK;
}
