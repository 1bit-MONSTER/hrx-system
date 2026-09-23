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

}  // namespace
