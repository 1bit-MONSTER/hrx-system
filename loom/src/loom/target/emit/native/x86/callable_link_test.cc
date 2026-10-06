// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <array>
#include <cstdint>

#include "iree/testing/gtest.h"

// These definitions are linked directly from the object emitted by Loom in a
// separate build action. The compiler process exits before the link begins.
extern "C" uint64_t mix(uint64_t input, uint64_t delta);
extern "C" uint64_t choose_mix(uint64_t input, uint64_t delta, uint64_t limit);
extern "C" uint64_t preserve(uint64_t a, uint64_t b, uint64_t c, uint64_t d,
                             uint64_t e, uint64_t f);
extern "C" uint64_t shift_mix(uint64_t unused_first, uint64_t count,
                              uint64_t unused_third, uint64_t input);
extern "C" uint64_t load_word(uint64_t unused, const uint64_t* input,
                              uint64_t index);
extern "C" uint64_t add_word(uint32_t word, uint64_t bias);
extern "C" uint32_t divide_mix(uint64_t unused_first, uint64_t unused_second,
                               uint32_t word);
extern "C" uint32_t replace_narrow(uint8_t* bytes, uint16_t* words,
                                   uint64_t index, uint32_t replacement);
extern "C" uint64_t high_mix(uint64_t unused, uint64_t factor, uint64_t word);
extern "C" uint64_t recurrence(uint64_t first, uint64_t second,
                               uint64_t iterations);

extern "C" uint64_t pressure64(const uint64_t* values);
extern "C" uint32_t pressure32(const uint32_t* values);
extern "C" uint64_t storage_spaces(uint64_t input, uint32_t word);
extern "C" uint64_t local_pair(uint64_t first, uint64_t second, uint64_t index);
extern "C" uint32_t sum_previous_instances(uint64_t count);

extern "C" uint64_t call_pair(uint64_t, uint64_t);
extern "C" uint64_t incoming_eight(uint64_t, uint64_t, uint64_t, uint64_t,
                                   uint64_t, uint64_t, uint64_t, uint64_t);
extern "C" uint64_t recursive_sum(uint64_t);

extern "C" uint64_t call_host(uint64_t x, uint64_t y);
extern "C" uint64_t native_host_mix(uint64_t x, uint64_t y) {
  return x * 37 + y * 19;
}
extern "C" uint64_t call_store(uint64_t* output, uint32_t word, uint64_t wide);
extern "C" void native_host_store(uint64_t* output, uint32_t a, uint64_t b,
                                  uint32_t c, uint64_t d, uint32_t e,
                                  uint64_t f, uint32_t g) {
  *output = uint64_t{a} + b + c + d + e + f + g;
}

extern "C" uint64_t reverse_eight(uint64_t, uint64_t, uint64_t, uint64_t,
                                  uint64_t, uint64_t, uint64_t, uint64_t);
extern "C" uint64_t incoming_aligned(uint64_t, uint64_t, uint64_t, uint64_t,
                                     uint64_t, uint64_t, uint64_t, uint64_t);
extern "C" uint64_t native_host_alignment(const void* pointer) {
  return reinterpret_cast<uintptr_t>(pointer) & 63;
}

extern "C" uint64_t call_fifteen(uint64_t seed);
extern "C" uint64_t native_host_fifteen(uint64_t a, uint64_t b, uint64_t c,
                                        uint64_t d, uint64_t e, uint64_t f,
                                        uint64_t g, uint64_t h, uint64_t i,
                                        uint64_t j, uint64_t k, uint64_t l,
                                        uint64_t m, uint64_t n, uint64_t o) {
  return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f + 7 * g + 8 * h + 9 * i +
         10 * j + 11 * k + 12 * l + 13 * m + 14 * n + 15 * o;
}
extern "C" uint64_t call_loop(uint64_t seed, uint64_t count);
extern "C" uint64_t saturated_permutation(uint64_t, uint64_t, uint64_t,
                                          uint64_t, uint64_t, uint64_t,
                                          uint64_t, uint64_t, uint64_t,
                                          uint64_t, uint64_t, uint64_t,
                                          uint64_t, uint64_t, uint32_t);

namespace {

TEST(NativeCallableTest, SaturatedLoopPermutation) {
  const std::array<uint64_t, 14> words = {UINT64_C(0x0123456789abcdef),
                                          UINT64_C(0xfedcba9876543210),
                                          3,
                                          5,
                                          8,
                                          13,
                                          21,
                                          34,
                                          55,
                                          89,
                                          144,
                                          233,
                                          377,
                                          610};
  uint64_t difference = 0;
  for (size_t i = 0; i < words.size(); i += 2) {
    difference += words[i] - words[i + 1];
  }
  for (uint32_t count = 0; count < 20; ++count) {
    EXPECT_EQ(saturated_permutation(words[0], words[1], words[2], words[3],
                                    words[4], words[5], words[6], words[7],
                                    words[8], words[9], words[10], words[11],
                                    words[12], words[13], count),
              count % 2 ? uint64_t{0} - difference : difference);
  }
}

TEST(NativeCallableTest, OrdinaryCLinkage) {
  uint64_t state = UINT64_C(0x243f6a8885a308d3);
  for (unsigned i = 0; i < 1000; ++i) {
    uint64_t words[6];
    for (uint64_t& word : words) {
      state = state * UINT64_C(6364136223846793005) + 1;
      word = state;
    }
    const uint64_t a = words[0], b = words[1], c = words[2];
    const uint64_t d = words[3], e = words[4], f = words[5];
    ASSERT_EQ(mix(a, b), (a + b) ^ a);
    ASSERT_EQ(choose_mix(a, b, c), a < c ? ((a + b) ^ a) : ((a * b) ^ c));
    ASSERT_EQ(preserve(a, b, c, d, e, f),
              ((a + b) * (c + d) + (e + f) * a) ^ (b ^ c));
    ASSERT_EQ(shift_mix(a, b, c, d), (d >> (b & 63)) ^ d);
    ASSERT_EQ(load_word(a, words, i % 6), words[i % 6]);
    ASSERT_EQ(add_word(static_cast<uint32_t>(a), b),
              static_cast<uint64_t>(static_cast<uint32_t>(a)) + b);
    const uint32_t word = static_cast<uint32_t>(a);
    ASSERT_EQ(divide_mix(b, c, word), ((word / 7) + (word % 7)) ^ word);
    const auto product = static_cast<unsigned __int128>(b) * c;
    ASSERT_EQ(high_mix(a, b, c), static_cast<uint64_t>(product >> 64) ^ c);
    uint64_t first = a, second = b;
    for (unsigned step = 0; step < i % 23; ++step) {
      const uint64_t sum = first + second;
      first = second;
      second = sum;
    }
    ASSERT_EQ(recurrence(a, b, i % 23), first);
  }
}

TEST(NativeCallableTest, NarrowMemoryPreservesNeighbors) {
  const std::array<uint8_t, 6> initial_bytes = {0, 127, 128, 255, 17, 201};
  const std::array<uint16_t, 6> initial_words = {0,     32767, 32768,
                                                 65535, 513,   54321};
  for (size_t index = 0; index < initial_bytes.size(); ++index) {
    auto bytes = initial_bytes;
    auto words = initial_words;
    const uint32_t previous = bytes[index] | (uint32_t{words[index]} << 8);
    const uint32_t replacement = 0xabcdef42u + static_cast<uint32_t>(index);
    auto expected_bytes = initial_bytes;
    auto expected_words = initial_words;
    expected_bytes[index] = static_cast<uint8_t>(replacement);
    expected_words[index] = static_cast<uint16_t>(replacement);
    ASSERT_EQ(replace_narrow(bytes.data(), words.data(), index, replacement),
              previous);
    EXPECT_EQ(bytes, expected_bytes);
    EXPECT_EQ(words, expected_words);
  }
}

TEST(NativeCallableTest, StackStorageAndAllocationSpills) {
  uint64_t state = UINT64_C(0x9e3779b97f4a7c15);
  for (unsigned repetition = 0; repetition < 100; ++repetition) {
    std::array<uint64_t, 20> wide;
    std::array<uint32_t, 20> narrow;
    uint64_t wide_sum = 0;
    uint32_t narrow_sum = 0;
    for (size_t i = 0; i < wide.size(); ++i) {
      state = state * UINT64_C(6364136223846793005) + 1;
      wide[i] = state;
      narrow[i] = static_cast<uint32_t>(state >> 32);
      wide_sum += wide[i];
      narrow_sum += narrow[i];
    }
    ASSERT_EQ(pressure64(wide.data()), wide_sum);
    ASSERT_EQ(pressure32(narrow.data()), narrow_sum);
    ASSERT_EQ(storage_spaces(wide[0], narrow[0]),
              wide[0] ^ (wide[0] + 258) ^ narrow[0]);
    ASSERT_EQ(local_pair(wide[0], wide[1], 0), wide[0]);
    ASSERT_EQ(local_pair(wide[0], wide[1], 1), wide[1]);
  }
}

TEST(NativeCallableTest, ReusesOnlyDeadAllocationInstances) {
  for (uint32_t count = 0; count < 20; ++count) {
    const uint32_t expected = count == 0 ? 0 : count * (count - 1) / 2;
    EXPECT_EQ(sum_previous_instances(count), expected);
  }
}

TEST(NativeCallableTest, CompleteCallOwnsItsOverflowAndAlignedLocal) {
  for (uint64_t x : {uint64_t{0}, uint64_t{11}, uint64_t{0x123456789abcdef0}}) {
    for (uint64_t y :
         {uint64_t{0}, uint64_t{23}, uint64_t{0xfedcba9876543210}}) {
      // The seven scalar fields have different weights; the local contributes
      // y to each call, independently of its first scalar argument.
      const uint64_t first = y + 23 * x + 33 * y + 19 * 5;
      const uint64_t second = y + 23 * y + 33 * x + 19 * 9;
      EXPECT_EQ(call_pair(x, y), first + second + (x ^ y));
    }
  }
}

TEST(NativeCallableTest, IncomingStackArgumentsFromIndependentCxxCaller) {
  EXPECT_EQ(incoming_eight(1, 2, 7, 9, 3, 5, 11, 13),
            ((uint64_t{1} + 2) + (7 ^ 9)) ^ (3 * 5 + 11 * 13));
  EXPECT_EQ(
      incoming_eight(9, 4, 13, 7, 3, 17, 0x100000001, 19),
      ((uint64_t{9} + 4) + (13 ^ 7)) ^ (3 * 17 + uint64_t{0x100000001} * 19));
}

TEST(NativeCallableTest, RecursiveFramesPreserveCallerValues) {
  for (uint64_t n : {uint64_t{0}, uint64_t{1}, uint64_t{17}, uint64_t{61}}) {
    EXPECT_EQ(recursive_sum(n), n * (n + 1) / 2);
  }
}

TEST(NativeCallableTest, ExternalCLinkage) {
  EXPECT_EQ(call_host(12345, 67890), (67890 * 37 + 12345 * 19) ^ 12345);
}

TEST(NativeCallableTest, MixedWidthVoidCall) {
  uint64_t output = 0;
  const uint32_t word = 0xfedcba98u;
  const uint64_t wide = UINT64_C(0x123456789abcdef0);
  const uint64_t expected = 4 * uint64_t{word} + 3 * wide;
  EXPECT_EQ(call_store(&output, word, wide), expected);
  EXPECT_EQ(output, expected);
}

TEST(NativeCallableTest, PermutesRegisterAndStackArguments) {
  const uint64_t a = 7, b = 23, c = 31, d = 43, e = 59, f = 67;
  const uint64_t g = UINT64_C(0x123456789abcdef0);
  const uint64_t h = UINT64_C(0xfedcba9876543210);
  EXPECT_EQ(reverse_eight(a, b, c, d, e, f, g, h),
            ((h + g) + (f ^ e)) ^ (d * c + b * a));
  EXPECT_EQ(incoming_aligned(a, b, c, d, e, f, g, h),
            h + 3 * a + 5 * b + 7 * c + 11 * d + 13 * e + 17 * f + 19 * g);
}

TEST(NativeCallableTest, ReusesConsumedOverflowForRegisterPermutation) {
  for (uint64_t seed : {uint64_t{0}, uint64_t{13}, UINT64_MAX}) {
    EXPECT_EQ(call_fifteen(seed), 1360 * seed);
  }
}

TEST(NativeCallableTest, CallsPreserveLoopState) {
  for (uint64_t count : {uint64_t{0}, uint64_t{1}, uint64_t{29}}) {
    const uint64_t seed = UINT64_C(0xfedcba9876543210);
    uint64_t expected = 0;
    for (uint64_t i = 0; i < count; ++i) {
      expected += 37 * (seed + i) + 19 * expected;
    }
    EXPECT_EQ(call_loop(seed, count), expected);
  }
}

}  // namespace
