// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "libamdf/src/platform/observation.h"

#include <windows.h>

amdf_status_t amdf_platform_observation_query_host_clock_info(
    amdf_observation_clock_info_t* out_info) {
  LARGE_INTEGER frequency;
  // QPC and QPF always succeed on the supported Windows versions.
  QueryPerformanceFrequency(&frequency);
  *out_info = (amdf_observation_clock_info_t){
      .frequency_hz = (uint64_t)frequency.QuadPart, .counter_bit_count = 64};
  return AMDF_STATUS_OK;
}

amdf_status_t amdf_platform_observation_query_host_clock(
    uint64_t* out_timestamp) {
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  *out_timestamp = (uint64_t)now.QuadPart;
  return AMDF_STATUS_OK;
}
