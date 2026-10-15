// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "gtest/gtest.h"
#include "xdna/xdna_device_fixture.h"

namespace {

class XdnaNativeLifetimeTest : public XdnaDeviceFixture {};

TEST_F(XdnaNativeLifetimeTest, ActivatesSelectedDevice) {
  EXPECT_NE(device_, nullptr);
}

}  // namespace
