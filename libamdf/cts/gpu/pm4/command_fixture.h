// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef AMDF_CTS_GPU_PM4_COMMAND_FIXTURE_H_
#define AMDF_CTS_GPU_PM4_COMMAND_FIXTURE_H_

#include "libamdf/cts/gpu/util/command_fixture.h"

// The native PM4 memory corpus uses explicit conservative system transitions.
// Admission requires both their wire format and the matching semantic roles.
class Pm4CommandTest : public GpuCommandTest {
 protected:
  Pm4CommandTest()
      : GpuCommandTest(AMDF_QUEUE_COMMAND_TYPE_GPU_PM4,
                       AMDF_QUEUE_ROLE_TRANSFER | AMDF_QUEUE_ROLE_CACHE_CONTROL,
                       AMDF_GPU_PM4_FORMAT_FEATURE_ACQUIRE_MEM_GCR,
                       AMDF_CACHE_OPERATIONS_RELEASE_TO_SYSTEM |
                           AMDF_CACHE_OPERATIONS_ACQUIRE_FROM_SYSTEM,
                       AMDF_CACHE_TRANSITION_KINDS_GLOBAL) {}
};

#endif  // AMDF_CTS_GPU_PM4_COMMAND_FIXTURE_H_
