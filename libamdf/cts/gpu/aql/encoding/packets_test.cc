// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "libamdf/cts/gpu/aql/encoding/packets.h"

#include <cstddef>

#include "gtest/gtest.h"

namespace {

TEST(AqlEncodingTest, BarrierSignalAddressesAndSystemScopes) {
  const auto packet =
      aql::Barrier(UINT64_C(0x1234567887654300), UINT64_C(0x2345678998765400));
  const aql::Packet expected = {0x1503, 0, 0x98765400, 0x23456789, 0, 0,
                                0,      0, 0,          0,          0, 0,
                                0,      0, 0x87654300, 0x12345678};
  EXPECT_EQ(packet, expected);
  EXPECT_EQ(offsetof(aql::Signal, value), 8u);
  EXPECT_EQ(alignof(aql::Signal), 64u);
  const auto no_dependency = aql::Barrier(UINT64_C(0x1234567887654300));
  EXPECT_EQ(no_dependency[2], 0u);
  EXPECT_EQ(no_dependency[3], 0u);
}

}  // namespace
