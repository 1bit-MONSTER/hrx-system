// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <array>
#include <cstdint>

#include "iree/testing/gtest.h"

// These symbols come from Loom's object, not from a C implementation. The
// normal C linker resolves them without any runtime or HAL adapter.
extern "C" {
uint64_t loom_test_native_arithmetic(uint64_t value, uint64_t scale,
                                     uint64_t bias);
void loom_test_native_copy_bytes(const void* source, uint64_t source_offset,
                                 void* target, uint64_t target_offset,
                                 uint64_t length);
void* loom_test_native_select_pointer(uint32_t condition, void* first,
                                      void* second);
uint32_t loom_test_native_shift_mix(uint32_t value, uint32_t count);
uint64_t loom_test_native_wide_shifts(uint64_t value, uint64_t left_count,
                                      uint64_t right_count);
uint64_t loom_test_native_sixth_argument(uint64_t a, uint64_t b, uint64_t c,
                                         uint64_t d, uint64_t e, uint64_t f);
uint64_t loom_test_native_constant();
uint64_t loom_test_native_branch(uint64_t condition, uint64_t first,
                                 uint64_t second);
uint64_t loom_test_native_high_products(uint64_t first, uint64_t second,
                                        uint64_t multiplier);
uint32_t loom_test_native_exchange(void* first, void* second,
                                   uint64_t first_origin,
                                   uint64_t second_origin, uint32_t condition,
                                   uint32_t replacement);
void loom_test_native_narrow_memory(const void* input, void* output,
                                    uint64_t index);
}

namespace {

TEST(NativeInteropTest, ArithmeticPreservesLiveInput) {
  constexpr uint64_t values[] = {0, 1, 37, UINT64_C(0x8000000000000000),
                                 UINT64_MAX};
  for (uint64_t value : values) {
    for (uint64_t scale : values) {
      for (uint64_t bias : values) {
        EXPECT_EQ(loom_test_native_arithmetic(value, scale, bias),
                  (value + bias) * scale + value);
      }
    }
  }
}

TEST(NativeInteropTest, LoopWritesOnlyTheRequestedRange) {
  std::array<uint8_t, 48> source;
  for (size_t i = 0; i < source.size(); ++i) {
    source[i] = i * 11 + 3;
  }
  for (uint64_t length : {0, 1, 7, 32}) {
    std::array<uint8_t, 48> target;
    target.fill(0xa5);
    loom_test_native_copy_bytes(source.data(), 5, target.data(), 9, length);
    for (size_t i = 0; i < target.size(); ++i) {
      const uint8_t expected = i >= 9 && i < 9 + length ? source[i - 4] : 0xa5;
      EXPECT_EQ(target[i], expected) << "length=" << length << " index=" << i;
    }
  }
}

TEST(NativeInteropTest, PointerAndRegisterBoundaries) {
  uint64_t first = 13;
  uint64_t second = 29;
  EXPECT_EQ(loom_test_native_select_pointer(0, &first, &second), &second);
  EXPECT_EQ(loom_test_native_select_pointer(1, &first, &second), &first);
  EXPECT_EQ(
      loom_test_native_select_pointer(UINT32_C(0x80000000), &first, &second),
      &first);
  EXPECT_EQ(loom_test_native_sixth_argument(1, 2, 3, 4, 5, 97), 97u);
  EXPECT_EQ(loom_test_native_constant(), UINT64_C(0x0123456789abcdef));
  EXPECT_EQ(loom_test_native_branch(0, first, second), second);
  EXPECT_EQ(loom_test_native_branch(UINT64_C(0x100000000), first, second),
            first);
}

TEST(NativeInteropTest, CountRegisterTransport) {
  for (uint32_t value : {0u, 1u, 0x12345678u, 0x80000000u, UINT32_MAX}) {
    for (uint32_t count : {0u, 1u, 7u, 31u, 32u, 63u}) {
      const uint32_t shift = count & 31;
      uint32_t arithmetic = value >> shift;
      if (shift && (value & 0x80000000u)) {
        arithmetic |= UINT32_MAX << (32 - shift);
      }
      const uint32_t mixed = (value << shift) ^ (value >> shift) ^ arithmetic;
      EXPECT_EQ(loom_test_native_shift_mix(value, count),
                (mixed + count) ^ value)
          << "value=" << value << " count=" << count;
    }
  }
}

TEST(NativeInteropTest, ImplicitMultiplyClobbers) {
  constexpr uint64_t values[] = {0, 1, UINT64_C(0xffffffff),
                                 UINT64_C(0x123456789abcdef0), UINT64_MAX};
  for (uint64_t first : values) {
    for (uint64_t second : values) {
      for (uint64_t multiplier : values) {
        const auto first_product =
            static_cast<unsigned __int128>(first) * multiplier;
        const auto second_product =
            static_cast<unsigned __int128>(second) * multiplier;
        EXPECT_EQ(loom_test_native_high_products(first, second, multiplier),
                  static_cast<uint64_t>(first_product >> 64) +
                      static_cast<uint64_t>(second_product >> 64));
      }
    }
  }
}

TEST(NativeInteropTest, WideCountRegisterTransport) {
  for (uint64_t value :
       {UINT64_C(0), UINT64_C(1), UINT64_C(0x8000000100000000), UINT64_MAX}) {
    for (uint64_t left_count : {0u, 1u, 32u, 63u, 64u}) {
      for (uint64_t right_count : {0u, 1u, 32u, 63u, 65u}) {
        const uint64_t shift = left_count & 63;
        const uint64_t left = value << shift;
        uint64_t arithmetic = left >> shift;
        if (shift && (left & UINT64_C(0x8000000000000000))) {
          arithmetic |= UINT64_MAX << (64 - shift);
        }
        const uint64_t logical = value >> (right_count & 63);
        EXPECT_EQ(loom_test_native_wide_shifts(value, left_count, right_count),
                  ((logical ^ arithmetic) + left_count + right_count) ^ value);
      }
    }
  }
}

TEST(NativeInteropTest, CorrelatedPointerAndOrigin) {
  for (uint32_t condition : {0u, 1u, 0x80000000u}) {
    std::array<uint8_t, 24> first;
    std::array<uint8_t, 24> second;
    first.fill(0x81);
    second.fill(0x93);
    EXPECT_EQ(loom_test_native_exchange(first.data(), second.data(), 5, 11,
                                        condition, 0x1234),
              condition ? 0x81u : 0x93u);
    for (size_t i = 0; i < first.size(); ++i) {
      EXPECT_EQ(first[i], condition && i == 5 ? 0x34 : 0x81);
      EXPECT_EQ(second[i], !condition && i == 11 ? 0x34 : 0x93);
    }
  }
}

TEST(NativeInteropTest, NarrowMemoryWidthsAndAddressScales) {
  std::array<uint8_t, 24> source;
  for (size_t i = 0; i < source.size(); ++i) {
    source[i] = i * 11 + 128;
  }
  for (uint64_t index : {1u, 7u}) {
    std::array<uint8_t, 24> target;
    target.fill(0xa5);
    auto expected = target;
    expected[index + 1] = source[3];
    expected[5] = source[index * 2 + 3];
    expected[6] = source[index * 2 + 4];
    loom_test_native_narrow_memory(source.data(), target.data(), index);
    EXPECT_EQ(target, expected);
  }
}

}  // namespace
