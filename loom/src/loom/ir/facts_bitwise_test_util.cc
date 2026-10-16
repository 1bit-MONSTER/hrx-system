// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/ir/facts_bitwise_test_util.h"

#include "iree/testing/gtest.h"

namespace loom::testing {

void CheckBoundedBitwiseConcreteResults(iree_host_size_t input_count,
                                        const loom_value_facts_t* inputs) {
  for (iree_host_size_t left_index = 0; left_index < input_count;
       ++left_index) {
    const loom_value_facts_t left = inputs[left_index];
    for (iree_host_size_t right_index = 0; right_index < input_count;
         ++right_index) {
      const loom_value_facts_t right = inputs[right_index];
      loom_value_facts_t result_and;
      loom_value_facts_t result_or;
      loom_value_facts_t result_xor;
      loom_value_facts_andi(&left, &right, &result_and);
      loom_value_facts_ori(&left, &right, &result_or);
      loom_value_facts_xori(&left, &right, &result_xor);
      ASSERT_GE(result_and.known_divisor, 1);
      ASSERT_GE(result_or.known_divisor, 1);
      ASSERT_GE(result_xor.known_divisor, 1);
      for (int64_t lhs = left.range_lo;; ++lhs) {
        if (lhs % left.known_divisor == 0) {
          for (int64_t rhs = right.range_lo;; ++rhs) {
            if (rhs % right.known_divisor == 0) {
              const int64_t value_and = lhs & rhs;
              const int64_t value_or = lhs | rhs;
              const int64_t value_xor = lhs ^ rhs;
              ASSERT_LE(result_and.range_lo, value_and) << lhs << " & " << rhs;
              ASSERT_GE(result_and.range_hi, value_and) << lhs << " & " << rhs;
              ASSERT_LE(result_or.range_lo, value_or) << lhs << " | " << rhs;
              ASSERT_GE(result_or.range_hi, value_or) << lhs << " | " << rhs;
              ASSERT_LE(result_xor.range_lo, value_xor) << lhs << " ^ " << rhs;
              ASSERT_GE(result_xor.range_hi, value_xor) << lhs << " ^ " << rhs;
              ASSERT_EQ(value_and % result_and.known_divisor, 0);
              ASSERT_EQ(value_or % result_or.known_divisor, 0);
              ASSERT_EQ(value_xor % result_xor.known_divisor, 0);
              ASSERT_TRUE(!loom_value_facts_is_non_zero(result_and) ||
                          value_and != 0);
              ASSERT_TRUE(!loom_value_facts_is_non_zero(result_or) ||
                          value_or != 0);
              ASSERT_TRUE(!loom_value_facts_is_non_zero(result_xor) ||
                          value_xor != 0);
            }
            if (rhs == right.range_hi) {
              break;
            }
          }
        }
        if (lhs == left.range_hi) {
          break;
        }
      }
    }
  }
}

}  // namespace loom::testing
