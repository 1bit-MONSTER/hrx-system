// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "libamdf/cts/gpu/pm4/encoding/commands.h"

#include <array>

#include "gtest/gtest.h"

namespace {

TEST(Pm4EncodingTest, MemoryWaitUsesFullAddressAndDwordPacketCount) {
  std::array<uint32_t, 16> words = {};
  words.back() = 0x24681357;
  Pm4CommandWriter commands(words.data());
  commands.WaitMemory32(UINT64_C(0x1234567887654320), 17);
  // WAIT_REG_MEM's type-3 count excludes two DWORDs; memory/equal control
  // and the full address are separate fields in the MEC wire format.
  const std::array<uint32_t, 7> expected = {
      0xc0053c00, 0x13, 0x87654320, 0x12345678, 17, 0xffffffff, 4};
  ASSERT_EQ(commands.word_count(), expected.size());
  for (size_t i = 0; i < expected.size(); ++i) {
    EXPECT_EQ(words[i], expected[i]) << i;
  }
  // One DWORD cannot hold a type-3 NOP, so padding advances to the next
  // eight-DWORD boundary with room for both the header and payload.
  commands.PadToEightWords();
  EXPECT_EQ(commands.word_count(), 16u);
  EXPECT_EQ(words[7], 0xc0071000u);
  EXPECT_EQ(words.back(), 0u);
}

}  // namespace
