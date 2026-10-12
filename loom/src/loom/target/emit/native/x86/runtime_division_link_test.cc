// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <array>
#include <cstdint>
#include <cstring>
#include <limits>

#include "iree/testing/gtest.h"

using U32x2 = uint32_t __attribute__((vector_size(8)));

extern "C" void runtime_division_i1(bool lhs, bool rhs, uint32_t* output);
extern "C" void runtime_unsigned_division_i1(bool lhs, bool rhs,
                                             uint32_t* output);
extern "C" bool signed_remainder_i1(bool lhs, bool rhs);
extern "C" void runtime_division_i8(uint8_t lhs, uint8_t rhs, uint32_t* output);
extern "C" uint8_t signed_remainder_i8(uint8_t lhs, uint8_t rhs);
extern "C" void runtime_division_i16(uint16_t lhs, uint16_t rhs,
                                     uint32_t* output);
extern "C" uint16_t signed_remainder_i16(uint16_t lhs, uint16_t rhs);
extern "C" void runtime_division_i32(uint32_t lhs, uint32_t rhs,
                                     uint32_t* output);
extern "C" uint32_t signed_remainder_i32(uint32_t lhs, uint32_t rhs);
extern "C" void runtime_division_i64(uint64_t lhs, uint64_t rhs,
                                     uint64_t* output);
extern "C" uint64_t signed_remainder_i64(uint64_t lhs, uint64_t rhs);
extern "C" void runtime_index_division(uint64_t lhs, uint64_t rhs,
                                       uint64_t* output);
extern "C" uint64_t division_pressure_i64(uint64_t a, uint64_t b, uint64_t c,
                                          uint64_t d, uint64_t e, uint64_t f);
extern "C" void mixed_fixed_register_division(uint64_t lhs64, uint64_t rhs64,
                                              uint32_t lhs32, uint64_t* output);
extern "C" void runtime_vector_division_u32x2(U32x2 lhs, U32x2 rhs,
                                              uint32_t* output);

namespace {

template <typename Signed, typename Unsigned>
Signed SignedFromBits(Unsigned bits) {
  static_assert(sizeof(Signed) == sizeof(Unsigned));
  Signed value;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

uint64_t NextRandom(uint64_t& state) {
  state = state * UINT64_C(6364136223846793005) + UINT64_C(1442695040888963407);
  return state;
}

template <typename Signed, typename Unsigned, typename Output>
void CheckDivisionCase(void (*function)(Unsigned, Unsigned, Output*),
                       Unsigned lhs, Unsigned rhs) {
  ASSERT_NE(rhs, Unsigned{0});
  const Signed signed_lhs = SignedFromBits<Signed>(lhs);
  const Signed signed_rhs = SignedFromBits<Signed>(rhs);
  ASSERT_FALSE(signed_lhs == std::numeric_limits<Signed>::min() &&
               signed_rhs == Signed{-1});

  std::array<Output, 4> output = {};
  function(lhs, rhs, output.data());
  EXPECT_EQ(output[0], static_cast<Output>(
                           static_cast<Unsigned>(signed_lhs / signed_rhs)));
  EXPECT_EQ(output[1], static_cast<Output>(
                           static_cast<Unsigned>(signed_lhs % signed_rhs)));
  EXPECT_EQ(output[2], static_cast<Output>(lhs / rhs));
  EXPECT_EQ(output[3], static_cast<Output>(lhs % rhs));
}

template <typename Signed, typename Unsigned, typename Output, size_t N>
void CheckDivisionFamily(void (*function)(Unsigned, Unsigned, Output*),
                         const std::array<Unsigned, N>& boundaries) {
  for (Unsigned lhs : boundaries) {
    for (Unsigned rhs : boundaries) {
      if (rhs == 0) {
        continue;
      }
      const Signed signed_lhs = SignedFromBits<Signed>(lhs);
      const Signed signed_rhs = SignedFromBits<Signed>(rhs);
      if (signed_lhs == std::numeric_limits<Signed>::min() &&
          signed_rhs == Signed{-1}) {
        continue;
      }
      CheckDivisionCase<Signed>(function, lhs, rhs);
    }
  }

  uint64_t state = UINT64_C(0x243f6a8885a308d3);
  for (size_t i = 0; i < 10000; ++i) {
    const Unsigned lhs = static_cast<Unsigned>(NextRandom(state));
    Unsigned rhs = static_cast<Unsigned>(NextRandom(state));
    if (rhs == 0) {
      rhs = 1;
    }
    const Signed signed_lhs = SignedFromBits<Signed>(lhs);
    const Signed signed_rhs = SignedFromBits<Signed>(rhs);
    if (signed_lhs == std::numeric_limits<Signed>::min() &&
        signed_rhs == Signed{-1}) {
      rhs = 1;
    }
    CheckDivisionCase<Signed>(function, lhs, rhs);
  }
}

TEST(NativeRuntimeDivisionTest, ScalarCarrierFamilies) {
  CheckDivisionFamily<int8_t, uint8_t, uint32_t>(
      runtime_division_i8,
      std::array<uint8_t, 6>{0, 1, 0x7f, 0x80, 0xfe, 0xff});
  CheckDivisionFamily<int16_t, uint16_t, uint32_t>(
      runtime_division_i16,
      std::array<uint16_t, 6>{0, 1, 0x7fff, 0x8000, 0xfffe, 0xffff});
  CheckDivisionFamily<int32_t, uint32_t, uint32_t>(
      runtime_division_i32,
      std::array<uint32_t, 6>{0, 1, UINT32_C(0x7fffffff), UINT32_C(0x80000000),
                              UINT32_C(0xfffffffe), UINT32_MAX});
  CheckDivisionFamily<int64_t, uint64_t, uint64_t>(
      runtime_division_i64,
      std::array<uint64_t, 6>{0, 1, UINT64_C(0x7fffffffffffffff),
                              UINT64_C(0x8000000000000000),
                              UINT64_C(0xfffffffffffffffe), UINT64_MAX});
}

TEST(NativeRuntimeDivisionTest, PredicateCarrierFamily) {
  std::array<uint32_t, 4> signed_output = {};
  runtime_division_i1(false, true, signed_output.data());
  EXPECT_EQ(signed_output, (std::array<uint32_t, 4>{0, 0, 0, 0}));

  std::array<uint32_t, 2> unsigned_output = {};
  runtime_unsigned_division_i1(true, true, unsigned_output.data());
  EXPECT_EQ(unsigned_output, (std::array<uint32_t, 2>{1, 0}));
  EXPECT_FALSE(signed_remainder_i1(true, true));
}

TEST(NativeRuntimeDivisionTest, SignedMinimumRemainderByMinusOneIsZero) {
  EXPECT_EQ(signed_remainder_i8(UINT8_C(0x80), UINT8_MAX), 0);
  EXPECT_EQ(signed_remainder_i16(UINT16_C(0x8000), UINT16_MAX), 0);
  EXPECT_EQ(signed_remainder_i32(UINT32_C(0x80000000), UINT32_MAX), 0u);
  EXPECT_EQ(signed_remainder_i64(UINT64_C(0x8000000000000000), UINT64_MAX), 0u);
}

TEST(NativeRuntimeDivisionTest, IndexUsesTheCompleteNonnegativeDomain) {
  uint64_t state = UINT64_C(0x13198a2e03707344);
  for (size_t i = 0; i < 10000; ++i) {
    const uint64_t lhs = NextRandom(state) & INT64_MAX;
    uint64_t rhs = NextRandom(state) & INT64_MAX;
    if (rhs == 0) {
      rhs = 1;
    }
    std::array<uint64_t, 2> output = {};
    runtime_index_division(lhs, rhs, output.data());
    EXPECT_EQ(output[0], lhs / rhs);
    EXPECT_EQ(output[1], lhs % rhs);
  }
}

TEST(NativeRuntimeDivisionTest, FixedRegistersPreserveLiveValues) {
  uint64_t state = UINT64_C(0xa4093822299f31d0);
  for (size_t i = 0; i < 10000; ++i) {
    const uint64_t a = NextRandom(state);
    uint64_t b = NextRandom(state);
    const uint64_t c = NextRandom(state);
    uint64_t d = NextRandom(state);
    const uint64_t e = NextRandom(state);
    uint64_t f = NextRandom(state);
    if (b == 0) {
      b = 1;
    }
    if (d == 0) {
      d = 1;
    }
    if (f == 0) {
      f = 1;
    }
    const uint64_t expected =
        (a / b) ^ (c % d) ^ (e / f) ^ a ^ b ^ c ^ d ^ e ^ f;
    EXPECT_EQ(division_pressure_i64(a, b, c, d, e, f), expected);
  }
}

TEST(NativeRuntimeDivisionTest, FixedRegisterWidthAliasesPreserveValues) {
  uint64_t state = UINT64_C(0x299f31d0082efa98);
  for (size_t i = 0; i < 10000; ++i) {
    const uint64_t lhs64 = NextRandom(state);
    uint64_t rhs64 = NextRandom(state);
    const uint32_t lhs32 = static_cast<uint32_t>(NextRandom(state));
    if (rhs64 == 0) {
      rhs64 = 1;
    }
    std::array<uint64_t, 4> output = {};
    mixed_fixed_register_division(lhs64, rhs64, lhs32, output.data());
    EXPECT_EQ(output[0], lhs64 / rhs64);
    EXPECT_EQ(output[1], lhs32 / 7u);
    EXPECT_EQ(output[2], lhs64 % rhs64);
    EXPECT_EQ(output[3], lhs32 % 7u);
  }
}

TEST(NativeRuntimeDivisionTest, SubnativeVectorScalarizesByLane) {
  uint64_t state = UINT64_C(0x082efa98ec4e6c89);
  for (size_t i = 0; i < 10000; ++i) {
    U32x2 lhs = {static_cast<uint32_t>(NextRandom(state)),
                 static_cast<uint32_t>(NextRandom(state))};
    U32x2 rhs = {static_cast<uint32_t>(NextRandom(state)),
                 static_cast<uint32_t>(NextRandom(state))};
    if (rhs[0] == 0) {
      rhs[0] = 1;
    }
    if (rhs[1] == 0) {
      rhs[1] = 1;
    }
    std::array<uint32_t, 4> output = {};
    runtime_vector_division_u32x2(lhs, rhs, output.data());
    EXPECT_EQ(output[0], lhs[0] / rhs[0]);
    EXPECT_EQ(output[1], lhs[1] / rhs[1]);
    EXPECT_EQ(output[2], lhs[0] % rhs[0]);
    EXPECT_EQ(output[3], lhs[1] % rhs[1]);
  }
}

}  // namespace
