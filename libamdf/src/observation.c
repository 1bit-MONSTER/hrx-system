// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "libamdf/src/observation.h"

#include "libamdf/src/platform/observation.h"

static amdf_status_t AMDF_CALL amdf_observation_query_host_clock_info(
    amdf_observation_clock_info_t* out_info) {
  if (out_info == NULL) {
    return amdf_make_api_status(AMDF_STATUS_CODE_INVALID_ARGUMENT);
  }
  return amdf_platform_observation_query_host_clock_info(out_info);
}

static amdf_status_t AMDF_CALL
amdf_observation_query_host_clock(uint64_t* out_timestamp) {
  if (out_timestamp == NULL) {
    return amdf_make_api_status(AMDF_STATUS_CODE_INVALID_ARGUMENT);
  }
  return amdf_platform_observation_query_host_clock(out_timestamp);
}

static const amdf_observation_api_t amdf_observation_api_v1 = {
    .structure_size = sizeof(amdf_observation_api_t),
    .extension_version = AMDF_OBSERVATION_EXTENSION_VERSION_1,
    .query_host_clock_info = amdf_observation_query_host_clock_info,
    .query_host_clock = amdf_observation_query_host_clock,
};

amdf_status_t amdf_observation_extension_query(uint32_t minimum_version,
                                               uint32_t maximum_version,
                                               const void** out_api) {
  if (minimum_version > AMDF_OBSERVATION_EXTENSION_VERSION_1 ||
      maximum_version < AMDF_OBSERVATION_EXTENSION_VERSION_1) {
    return amdf_make_api_status(AMDF_STATUS_CODE_VERSION_MISMATCH);
  }
  *out_api = &amdf_observation_api_v1;
  return AMDF_STATUS_OK;
}
