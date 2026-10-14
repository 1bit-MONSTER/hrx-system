// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "libamdf/src/xdna/umd/clock.h"

#include <drm/amdxdna_accel.h>
#include <sys/ioctl.h>

#include "libamdf/src/platform/linux/file.h"
#include "libamdf/src/xdna/umd/drm/device.h"

amdf_status_t amdf_xdna_umd_query_timer_clock_info(
    amdf_xdna_umd_device_t* device, amdf_observation_clock_info_t* out_info) {
  struct amdxdna_drm_query_clock_metadata metadata = {0};
  struct amdxdna_drm_get_info request = {
      .param = DRM_AMDXDNA_QUERY_CLOCK_METADATA,
      .buffer_size = sizeof(metadata),
      .buffer = (uintptr_t)&metadata,
  };
  if (ioctl(device->descriptor, DRM_IOCTL_AMDXDNA_GET_INFO, &request) != 0) {
    return errno == EOPNOTSUPP || errno == ENOTTY
               ? amdf_make_api_status(AMDF_STATUS_CODE_UNSUPPORTED)
               : amdf_linux_error(errno);
  }
  if (metadata.h_clock.freq_mhz == 0) {
    return amdf_make_api_status(AMDF_STATUS_CODE_UNSUPPORTED);
  }
  // The partition-shim timer uses the array H clock, not the separate MP-NPU
  // controller clock. Native telemetry/configuration supplies an operating
  // snapshot in MHz; neither this query nor a result-buffer attachment pins it.
  *out_info = (amdf_observation_clock_info_t){
      .frequency_hz = (uint64_t)metadata.h_clock.freq_mhz * 1000000,
      .counter_bit_count = 64,
  };
  return AMDF_STATUS_OK;
}
