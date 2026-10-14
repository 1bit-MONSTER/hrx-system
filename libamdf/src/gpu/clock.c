// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "libamdf/src/gpu/clock.h"

#include "libamdf/src/device.h"
#include "libamdf/src/endpoint.h"
#include "libamdf/src/gpu/device.h"
#include "libamdf/src/gpu/umd/clock.h"

amdf_status_t AMDF_CALL amdf_gpu_device_query_queue_clock_info(
    amdf_device_t* device, uint32_t queue_family_ordinal,
    amdf_observation_clock_info_t* out_info) {
  if (out_info == NULL || device == NULL) {
    return amdf_make_api_status(AMDF_STATUS_CODE_INVALID_ARGUMENT);
  }
  if (!amdf_device_is_engine(device, AMDF_ENGINE_KIND_GPU)) {
    return amdf_make_api_status(AMDF_STATUS_CODE_UNSUPPORTED);
  }
  amdf_queue_family_info_t family = {
      .type = AMDF_STRUCTURE_TYPE_QUEUE_FAMILY_INFO,
      .structure_size = sizeof(family),
  };
  const amdf_status_t status = amdf_endpoint_query_queue_family_info(
      device->endpoint, queue_family_ordinal, &family);
  if (!amdf_status_is_ok(status)) {
    return status;
  }
  return amdf_gpu_umd_query_queue_clock_info(amdf_gpu_device_get_umd(device),
                                             family.command_type, out_info);
}
