// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "libamdf/cts/gpu/sdma/encoding/commands.h"

#include <array>

#include "gtest/gtest.h"

namespace {

TEST(SdmaEncodingTest, LinearByteCountAndUncachedCompletion) {
  std::array<uint32_t, 12> words = {};
  words.back() = 0x24681357;
  SdmaCommandWriter commands(words.data(), 0);
  commands.CopyLinear(UINT64_C(0x1234567887654320),
                      UINT64_C(0x2345678998765430), 1028);
  commands.Fence32(UINT64_C(0x34567890abcdef00), 19);
  // COPY_LINEAR carries bytes-minus-one; FENCE is an independent four-DWORD
  // packet, with uncached MTYPE and no implicit GCR or scope fields.
  const std::array<uint32_t, 11> expected = {
      1,          1027,       0,          0x87654320, 0x12345678, 0x98765430,
      0x23456789, 0x00030005, 0xabcdef00, 0x34567890, 19};
  ASSERT_EQ(commands.word_count(), expected.size());
  for (size_t i = 0; i < expected.size(); ++i) {
    EXPECT_EQ(words[i], expected[i]) << i;
  }
  EXPECT_EQ(words.back(), 0x24681357u);
}

TEST(SdmaEncodingTest, GlobalTimestampUsesFullAddressAndNoImplicitFence) {
  std::array<uint32_t, 4> words = {0, 0, 0, 0x9876abcd};
  SdmaCommandWriter commands(words.data(), 0);
  commands.WriteGlobalTimestamp(UINT64_C(0x12345678abcdef20));
  // ROCr SDMA_OP_TIMESTAMP=13 / GET_GLOBAL=2 and PAL's unscoped
  // SDMA_PKT_TIMESTAMP_GET_GLOBAL have a header plus full destination address.
  const std::array<uint32_t, 3> expected = {0x0000020d, 0xabcdef20, 0x12345678};
  ASSERT_EQ(commands.word_count(), expected.size());
  for (size_t i = 0; i < expected.size(); ++i) {
    EXPECT_EQ(words[i], expected[i]) << i;
  }
  EXPECT_EQ(words.back(), 0x9876abcdu);
}

TEST(SdmaEncodingTest, LinearShortTransfersKeepByteCountUnits) {
  for (uint32_t byte_length : {1u, 2u, 3u, 4u, 31u, 4101u}) {
    SCOPED_TRACE(byte_length);
    std::array<uint32_t, 8> words = {};
    words.back() = 0x31415926;
    SdmaCommandWriter commands(words.data(), 0);
    commands.CopyLinear(UINT64_C(0x1234567800000fff),
                        UINT64_C(0x2345678900001003), byte_length);
    ASSERT_EQ(commands.word_count(), 7u);
    EXPECT_EQ(words[0], 1u);
    EXPECT_EQ(words[1], byte_length - 1);
    EXPECT_EQ(words[2], 0u);
    EXPECT_EQ(words[3], 0x00000fffu);
    EXPECT_EQ(words[4], 0x12345678u);
    EXPECT_EQ(words[5], 0x00001003u);
    EXPECT_EQ(words[6], 0x23456789u);
    EXPECT_EQ(words.back(), 0x31415926u);
  }
}

}  // namespace
