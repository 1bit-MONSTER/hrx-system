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
  const auto packet = aql::Barrier(
      aql::BarrierType::kAnd, aql::HeaderBarrier::kEnabled,
      UINT64_C(0x1234567887654300), {UINT64_C(0x2345678998765400)});
  // HSA System Architecture 1.2, tables 2-4 through 2-6 and 2-9:
  // AND=3, barrier bit 8, SYSTEM=2 at bits 9 and 11; dependency byte 8,
  // completion byte 56. These literal words do not use the encoder's enums.
  const aql::Packet expected = {0x1503, 0, 0x98765400, 0x23456789, 0, 0,
                                0,      0, 0,          0,          0, 0,
                                0,      0, 0x87654300, 0x12345678};
  EXPECT_EQ(packet, expected);
  // ROCm 8d57824901ff, amd_hsa_signal.h, amd_signal_t.
  EXPECT_EQ(offsetof(aql::Signal, kind), 0u);
  EXPECT_EQ(offsetof(aql::Signal, value), 8u);
  EXPECT_EQ(sizeof(aql::Signal), 64u);
  EXPECT_EQ(alignof(aql::Signal), 64u);
}

TEST(AqlEncodingTest, BarrierAndEncodesAllFiveDependencies) {
  const auto packet =
      aql::Barrier(aql::BarrierType::kAnd, aql::HeaderBarrier::kDisabled,
                   UINT64_C(0x1234567887654300),
                   {UINT64_C(0x2345678998765400), UINT64_C(0x3456789aa9876500),
                    UINT64_C(0x456789abba987600), UINT64_C(0x56789abccba98700),
                    UINT64_C(0x6789abcddcba9800)});
  // HSA System Architecture 1.2 table 2-9 places five 64-bit handles at
  // bytes 8, 16, 24, 32 and 40; bytes 48 through 55 remain reserved zero.
  const aql::Packet expected = {0x1403,     0,          0x98765400, 0x23456789,
                                0xa9876500, 0x3456789a, 0xba987600, 0x456789ab,
                                0xcba98700, 0x56789abc, 0xdcba9800, 0x6789abcd,
                                0,          0,          0x87654300, 0x12345678};
  EXPECT_EQ(packet, expected);
}

TEST(AqlEncodingTest, BarrierOrEncodesSparseDependenciesAndNullCompletion) {
  const auto packet = aql::Barrier(
      aql::BarrierType::kOr, aql::HeaderBarrier::kDisabled, 0,
      {0, UINT64_C(0x1234567887654300), 0, UINT64_C(0x2345678998765400), 0});
  // HSA System Architecture 1.2 tables 2-4 and 2-10 define OR=5 with the
  // same layout as AND. Null dependency handles do not satisfy OR.
  const aql::Packet expected = {
      0x1405,     0,          0, 0, 0x87654300, 0x12345678, 0, 0,
      0x98765400, 0x23456789, 0, 0, 0,          0,          0, 0};
  EXPECT_EQ(packet, expected);
}

TEST(AqlEncodingTest, BarrierAndAllowsNullDependenciesAndCompletion) {
  const auto packet =
      aql::Barrier(aql::BarrierType::kAnd, aql::HeaderBarrier::kEnabled, 0);
  // HSA System Architecture 1.2 sections 2.9.2 and 2.9.8 permit no completion
  // signal and treat every null AND dependency as satisfied.
  const aql::Packet expected = {0x1503, 0, 0, 0, 0, 0, 0, 0,
                                0,      0, 0, 0, 0, 0, 0, 0};
  EXPECT_EQ(packet, expected);
}

}  // namespace
