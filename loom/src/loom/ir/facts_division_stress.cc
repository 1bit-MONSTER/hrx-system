// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <array>
#include <bitset>
#include <climits>
#include <cstdint>
#include <vector>

#include "iree/testing/gtest.h"
#include "loom/ir/facts.h"
#include "loom/ir/facts_division_test_util.h"

namespace loom {
namespace {

TEST(FactsDivisionStressTest, ExactUnsignedSmallWidths) {
  for (int32_t bit_count = 1; bit_count <= 8; ++bit_count) {
    const uint64_t mask = iree_math_mask_low_bits_u64(UINT64_MAX, bit_count);
    for (uint64_t dividend_bits = 0; dividend_bits <= mask; ++dividend_bits) {
      const auto dividend = testing::ExactIntegerBits(dividend_bits, bit_count);
      for (uint64_t divisor_bits = 1; divisor_bits <= mask; ++divisor_bits) {
        SCOPED_TRACE(::testing::Message()
                     << "width=" << bit_count << " dividend=" << dividend_bits
                     << " divisor=" << divisor_bits);
        const auto divisor = testing::ExactIntegerBits(divisor_bits, bit_count);
        EXPECT_EQ(
            loom_value_facts_remui_is_identity(dividend, divisor, bit_count),
            dividend_bits < divisor_bits);
        loom_value_facts_t quotient;
        loom_value_facts_divui(&dividend, &divisor, bit_count, &quotient);
        EXPECT_TRUE(loom_value_facts_is_exact(quotient));
        EXPECT_EQ(quotient.range_lo,
                  testing::SignedIntegerRepresentation(
                      dividend_bits / divisor_bits, bit_count));

        loom_value_facts_t remainder;
        loom_value_facts_remui(&dividend, &divisor, bit_count, &remainder);
        EXPECT_TRUE(loom_value_facts_is_exact(remainder));
        EXPECT_EQ(remainder.range_lo,
                  testing::SignedIntegerRepresentation(
                      dividend_bits % divisor_bits, bit_count));
      }
    }
  }
}

TEST(FactsDivisionStressTest, FinitePreimagesContainEveryAdmittedDividend) {
  for (int32_t bit_count : {1, 4, 8}) {
    const int64_t minimum =
        bit_count == 1 ? 0 : -(INT64_C(1) << (bit_count - 1));
    const int64_t maximum =
        bit_count == 1 ? 1 : (INT64_C(1) << (bit_count - 1)) - 1;
    std::vector<loom_value_facts_t> intervals;
    for (int64_t lower = minimum; lower <= maximum; ++lower) {
      for (int64_t upper = lower; upper <= maximum; ++upper) {
        if (bit_count != 8 || lower == minimum || upper == maximum ||
            lower == upper) {
          intervals.push_back(loom_value_facts_make(lower, upper, 1));
        }
      }
    }
    std::vector<loom_value_facts_t> scales = intervals;
    if (bit_count == 8) {
      scales.clear();
      for (int64_t value :
           {-128, -127, -33, -32, -3, -1, 0, 1, 2, 3, 31, 32, 33, 126, 127}) {
        scales.push_back(loom_value_facts_exact_i64(value));
      }
      for (auto range : {std::pair{-128, -1}, std::pair{-3, 3},
                         std::pair{1, 127}, std::pair{16, 32}}) {
        scales.push_back(loom_value_facts_make(range.first, range.second, 1));
      }
    }
    for (auto kind : testing::kQuotientKinds) {
      for (auto scale : scales) {
        std::array<std::bitset<256>, 256> outputs;
        for (int64_t dividend = minimum; dividend <= maximum; ++dividend) {
          auto& possible = outputs[dividend - minimum];
          for (int64_t value = scale.range_lo; value <= scale.range_hi;
               ++value) {
            int64_t quotient = 0;
            if (testing::EvaluateQuotient(kind, bit_count, dividend, value,
                                          &quotient)) {
              possible.set(quotient - minimum);
            } else {
              possible.set();
              break;
            }
          }
        }
        for (auto quotient : intervals) {
          const auto preimage = loom_value_facts_quotient_preimage(
              kind, bit_count, quotient, scale);
          std::bitset<256> admitted;
          for (int64_t value = quotient.range_lo; value <= quotient.range_hi;
               ++value) {
            admitted.set(value - minimum);
          }
          for (int64_t dividend = minimum; dividend <= maximum; ++dividend) {
            if ((outputs[dividend - minimum] & admitted).any()) {
              ASSERT_TRUE(dividend >= preimage.range_lo &&
                          dividend <= preimage.range_hi)
                  << "kind=" << kind << " width=" << bit_count << " quotient=["
                  << quotient.range_lo << "," << quotient.range_hi
                  << "] scale=[" << scale.range_lo << "," << scale.range_hi
                  << "] preimage=[" << preimage.range_lo << ","
                  << preimage.range_hi << "] dividend=" << dividend;
            }
          }
        }
      }
    }
  }
}

}  // namespace
}  // namespace loom
