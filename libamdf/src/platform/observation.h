// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef AMDF_SRC_PLATFORM_OBSERVATION_H_
#define AMDF_SRC_PLATFORM_OBSERVATION_H_

#include "amdf/observation.h"

// Queries the profiling host clock used by native GPU correlation samples.
// The caller supplies valid output storage; failure leaves it unchanged.
amdf_status_t amdf_platform_observation_query_host_clock_info(
    amdf_observation_clock_info_t* out_info);

// Reads CLOCK_MONOTONIC_RAW on Linux or QPC on Windows in its native units.
// The caller supplies valid output storage; failure leaves it unchanged.
amdf_status_t amdf_platform_observation_query_host_clock(
    uint64_t* out_timestamp);

#endif  // AMDF_SRC_PLATFORM_OBSERVATION_H_
