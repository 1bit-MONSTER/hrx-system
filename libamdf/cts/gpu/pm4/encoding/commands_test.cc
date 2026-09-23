// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "libamdf/cts/gpu/pm4/encoding/commands.h"

#include <array>
#include <vector>

#include "gtest/gtest.h"

namespace {

// Literal wire expectations are transcribed from PAL's MEC packet layouts and
// independently checked against Mesa's CP builders. No production constants or
// bitfield definitions are shared with the encoder; the source ledger is in
// docs/reference/amd/gpu/pm4/memory-commands.md.
template <size_t N>
void ExpectWords(const uint32_t* words,
                 const std::array<uint32_t, N>& expected) {
  for (size_t i = 0; i < expected.size(); ++i) {
    EXPECT_EQ(words[i], expected[i]) << "word " << i;
  }
}

TEST(Pm4EncodingTest, ConfirmedCopiesPreserveAddressesAndSelectWidth) {
  std::array<uint32_t, 13> words = {};
  words.back() = 0x24681357;
  Pm4CommandWriter commands(words.data());
  commands.CopyData32(UINT64_C(0x0000123487654324),
                      UINT64_C(0x00005678fedcba94));
  commands.CopyData64(UINT64_C(0x0000123487654328),
                      UINT64_C(0x00005678fedcba98));
  const std::array<uint32_t, 12> expected = {
      0xc0044000, 0x00100202, 0x87654324, 0x00001234, 0xfedcba94, 0x00005678,
      0xc0044000, 0x00110202, 0x87654328, 0x00001234, 0xfedcba98, 0x00005678};
  ASSERT_EQ(commands.word_count(), expected.size());
  ExpectWords(words.data(), expected);
  EXPECT_EQ(words.back(), 0x24681357u);
}

TEST(Pm4EncodingTest, IncrementingWriteIncludesEveryPayloadWordInCount) {
  std::array<uint32_t, 13> words = {};
  words.back() = 0x24681357;
  const std::array<uint32_t, 3> values = {0, UINT32_MAX, 0x13579bdf};
  Pm4CommandWriter commands(words.data());
  commands.WriteData32(UINT64_C(0x0000123487654324), 0x2468ace0);
  commands.WriteData(UINT64_C(0x00005678fedcba94), values.data(),
                     values.size());
  const std::array<uint32_t, 12> expected = {
      0xc0033700, 0x00100200, 0x87654324, 0x00001234, 0x2468ace0, 0xc0053700,
      0x00100200, 0xfedcba94, 0x00005678, 0,          UINT32_MAX, 0x13579bdf};
  ASSERT_EQ(commands.word_count(), expected.size());
  ExpectWords(words.data(), expected);
  EXPECT_EQ(words.back(), 0x24681357u);
}

TEST(Pm4EncodingTest, MaximumWritePayloadUsesAllFourteenCountBits) {
  // A 14-bit count holds total DWORDs minus two. Four fixed DWORDs leave
  // 16381 payload DWORDs. This is a wire-format boundary, not a native
  // workload.
  std::vector<uint32_t> values(16381, 0x13579bdf);
  std::vector<uint32_t> words(16386, 0x24681357);
  Pm4CommandWriter commands(words.data());
  commands.WriteData(UINT64_C(0x0000123487654324), values.data(),
                     values.size());
  ASSERT_EQ(commands.word_count(), 16385u);
  const std::array<uint32_t, 4> prefix = {0xffff3700, 0x00100200, 0x87654324,
                                          0x00001234};
  ExpectWords(words.data(), prefix);
  for (size_t i = 0; i < values.size(); ++i) {
    EXPECT_EQ(words[4 + i], values[i]) << i;
  }
  EXPECT_EQ(words.back(), 0x24681357u);
}

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
  // The selected ordinary NOP form needs a header and payload, so padding
  // advances to the next eight-DWORD boundary. The special one-DWORD NOP
  // encoding is a separate form that this emitter does not use.
  commands.PadToEightWords();
  EXPECT_EQ(commands.word_count(), 16u);
  EXPECT_EQ(words[7], 0xc0071000u);
  EXPECT_EQ(words.back(), 0u);
}

TEST(Pm4EncodingTest, MaskedMemoryWaitsKeepOrdinaryMecExecution) {
  std::array<uint32_t, 22> words = {};
  words.back() = 0x24681357;
  Pm4CommandWriter commands(words.data());
  commands.WaitMemory32(UINT64_C(0x0000123487654324), 0x80000000,
                        Pm4MemoryComparison::kEqual, 0xff000000);
  commands.WaitMemory32(UINT64_C(0x00005678fedcba94), 0,
                        Pm4MemoryComparison::kNotEqual, 0x80000000);
  commands.WaitMemory32(UINT64_C(0x00009abcedcba984), 0x80000001,
                        Pm4MemoryComparison::kGreaterOrEqual);
  const std::array<uint32_t, 21> expected = {
      0xc0053c00, 0x13, 0x87654324, 0x00001234, 0x80000000, 0xff000000, 4,
      0xc0053c00, 0x14, 0xfedcba94, 0x00005678, 0,          0x80000000, 4,
      0xc0053c00, 0x15, 0xedcba984, 0x00009abc, 0x80000001, UINT32_MAX, 4};
  ASSERT_EQ(commands.word_count(), expected.size());
  ExpectWords(words.data(), expected);
  EXPECT_EQ(words.back(), 0x24681357u);
}

TEST(Pm4EncodingTest, WideWaitSeparatesReferenceAndMaskHalves) {
  std::array<uint32_t, 10> words = {};
  words.back() = 0x24681357;
  Pm4CommandWriter commands(words.data());
  commands.WaitMemory64(
      UINT64_C(0x0000123487654328), UINT64_C(0x8765000012340000),
      Pm4MemoryComparison::kGreaterOrEqual, UINT64_C(0xffff0000ffff0000));
  const std::array<uint32_t, 9> expected = {0xc0079300, 0x15,       0x87654328,
                                            0x00001234, 0x12340000, 0x87650000,
                                            0xffff0000, 0xffff0000, 4};
  ASSERT_EQ(commands.word_count(), expected.size());
  ExpectWords(words.data(), expected);
  EXPECT_EQ(words.back(), 0x24681357u);
}

TEST(Pm4EncodingTest, GpuClockCopyUsesConfirmedWideTimestampSource) {
  std::array<uint32_t, 7> words = {};
  words.back() = 0x24681357;
  Pm4CommandWriter commands(words.data());
  commands.CopyGpuClock64(UINT64_C(0x0000123487654328));
  const std::array<uint32_t, 6> expected = {0xc0044000, 0x00110509, 0,
                                            0,          0x87654328, 0x00001234};
  ASSERT_EQ(commands.word_count(), expected.size());
  ExpectWords(words.data(), expected);
  EXPECT_EQ(words.back(), 0x24681357u);
}

TEST(Pm4EncodingTest, SystemBarrierKeepsMecSizeAndGcrFieldsSeparate) {
  std::array<uint32_t, 11> words = {};
  words.back() = 0x24681357;
  Pm4CommandWriter commands(words.data());
  commands.SystemBarrier();
  // The MEC size-high field is eight bits. The remaining high bits stay
  // reserved even though some graphics-engine forms have a wider field.
  const std::array<uint32_t, 10> expected = {
      0xc0004600, 0x00000407, 0xc0065800, 0,          UINT32_MAX,
      0x000000ff, 0,          0,          0x0000000a, 0x0000c3a1};
  ASSERT_EQ(commands.word_count(), expected.size());
  ExpectWords(words.data(), expected);
  EXPECT_EQ(words.back(), 0x24681357u);
}

TEST(Pm4EncodingTest, PaddingUsesWholePacketsAtEveryResidue) {
  constexpr std::array<size_t, 8> kPaddingWords = {8, 7, 6, 5, 4, 3, 2, 9};
  constexpr std::array<uint32_t, 8> kPaddingHeaders = {
      0xc0061000, 0xc0051000, 0xc0041000, 0xc0031000,
      0xc0021000, 0xc0011000, 0xc0001000, 0xc0071000};
  const std::array<uint32_t, 8> values = {};
  for (size_t value_count = 1; value_count <= values.size(); ++value_count) {
    SCOPED_TRACE(value_count);
    std::array<uint32_t, 25> words;
    words.fill(0x24681357);
    Pm4CommandWriter commands(words.data());
    commands.WriteData(UINT64_C(0x0000123487654324), values.data(),
                       value_count);
    const size_t prefix_count = commands.word_count();
    const size_t residue = prefix_count % 8;
    commands.PadToEightWords();
    ASSERT_EQ(commands.word_count(), prefix_count + kPaddingWords[residue]);
    EXPECT_EQ(commands.word_count() % 8, 0u);
    EXPECT_EQ(words[prefix_count], kPaddingHeaders[residue]);
    for (size_t i = prefix_count + 1; i < commands.word_count(); ++i) {
      EXPECT_EQ(words[i], 0u) << i;
    }
    EXPECT_EQ(words[commands.word_count()], 0x24681357u);
  }
}

}  // namespace
