// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "libamdf/src/gpu/umd/clock.h"

#include "libamdf/src/gpu/umd/wddm/device.h"

amdf_status_t amdf_gpu_umd_query_queue_clock_info(
    amdf_gpu_umd_device_t* device, amdf_queue_command_type_t command_type,
    amdf_observation_clock_info_t* out_info) {
  const amdf_wkmi_bridge_gpu_queue_command_type_t native_type =
      command_type == AMDF_QUEUE_COMMAND_TYPE_GPU_PM4
          ? AMDF_WKMI_BRIDGE_GPU_QUEUE_COMMAND_TYPE_PM4
          : AMDF_WKMI_BRIDGE_GPU_QUEUE_COMMAND_TYPE_SDMA;
  uint64_t frequency = 0;
  const amdf_status_t status =
      amdf_gpu_wddm_wkmi_adapter_query_queue_clock_frequency(
          &device->wkmi_adapter, device->adapter,
          device->physical_adapter_index, native_type, &frequency);
  if (amdf_status_is_ok(status)) {
    *out_info = (amdf_observation_clock_info_t){.frequency_hz = frequency};
  }
  return status;
}
