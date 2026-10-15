// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#define _POSIX_C_SOURCE 200809L
#include "libamdf/src/platform/observation.h"

#include <time.h>

#include "libamdf/src/platform/linux/file.h"

amdf_status_t amdf_platform_observation_query_host_clock_info(
    amdf_observation_clock_info_t* out_info) {
  *out_info = (amdf_observation_clock_info_t){
      .frequency_hz = UINT64_C(1000000000), .counter_bit_count = 64};
  return AMDF_STATUS_OK;
}

amdf_status_t amdf_platform_observation_query_host_clock(
    uint64_t* out_timestamp) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC_RAW, &now) != 0) {
    return amdf_linux_error(errno);
  }
  *out_timestamp = (uint64_t)now.tv_sec * UINT64_C(1000000000) + now.tv_nsec;
  return AMDF_STATUS_OK;
}
