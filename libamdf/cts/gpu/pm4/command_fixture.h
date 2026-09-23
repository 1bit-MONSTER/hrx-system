// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef AMDF_CTS_GPU_PM4_COMMAND_FIXTURE_H_
#define AMDF_CTS_GPU_PM4_COMMAND_FIXTURE_H_

#include "libamdf/cts/gpu/pm4/encoding/commands.h"
#include "libamdf/cts/gpu/util/command_fixture.h"

// Admits the GFX11.0/GFX11.5 conservative memory recipe, its semantic queue
// requirements and user publication before borrowing the cached native device.
class Pm4CommandTest : public GpuCommandTest {
 protected:
  Pm4CommandTest()
      : GpuCommandTest(AMDF_QUEUE_COMMAND_TYPE_GPU_PM4,
                       AMDF_QUEUE_ROLE_TRANSFER | AMDF_QUEUE_ROLE_CACHE_CONTROL,
                       AMDF_GPU_PM4_FORMAT_FEATURE_ACQUIRE_MEM_GCR,
                       AMDF_CACHE_OPERATIONS_RELEASE_TO_SYSTEM |
                           AMDF_CACHE_OPERATIONS_ACQUIRE_FROM_SYSTEM,
                       AMDF_CACHE_TRANSITION_KINDS_GLOBAL) {}

  amdf_status_t MatchGpuEndpoint(amdf_endpoint_t* endpoint,
                                 bool* out_matches) override {
    amdf_gpu_endpoint_info_t info = {};
    info.type = AMDF_STRUCTURE_TYPE_GPU_ENDPOINT_INFO;
    info.structure_size = sizeof(info);
    const amdf_status_t status = gpu_api_->endpoint_query_info(endpoint, &info);
    if (!amdf_status_is_ok(status)) {
      return status;
    }
    if (!Pm4CommandWriter::SupportsTarget(info)) {
      *out_matches = false;
      return AMDF_STATUS_OK;
    }
    return GpuCommandTest::MatchGpuEndpoint(endpoint, out_matches);
  }
};

#endif  // AMDF_CTS_GPU_PM4_COMMAND_FIXTURE_H_
