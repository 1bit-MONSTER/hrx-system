// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "gpu/gpu_device_fixture.h"
#include "gtest/gtest.h"

namespace {

class GpuNativeLifetimeTest : public GpuDeviceFixture {};

TEST_F(GpuNativeLifetimeTest, ActivatesSelectedDevice) {
  EXPECT_NE(device_, nullptr);
}

}  // namespace
