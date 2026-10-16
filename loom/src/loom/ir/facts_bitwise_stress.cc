// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <cstdint>
#include <vector>

#include "iree/testing/gtest.h"
#include "loom/ir/facts.h"
#include "loom/ir/facts_bitwise_test_util.h"

namespace loom {
namespace {

TEST(FactsBitwiseStressTest,
     BoundedIntervalsAndDivisorsContainEveryConcreteResult) {
  std::vector<loom_value_facts_t> inputs;
  for (int64_t lower = -8; lower <= 8; ++lower) {
    for (int64_t upper = lower; upper <= 8; ++upper) {
      for (int64_t divisor = 1; divisor <= 8; ++divisor) {
        // Exact constructors derive their divisor from the value.
        if (lower == upper && divisor != 1) {
          continue;
        }
        bool has_value = false;
        for (int64_t value = lower; value <= upper; ++value) {
          has_value |= value % divisor == 0;
        }
        if (has_value) {
          inputs.push_back(loom_value_facts_make(lower, upper, divisor));
        }
      }
    }
  }
  testing::CheckBoundedBitwiseConcreteResults(inputs.size(), inputs.data());
}

}  // namespace
}  // namespace loom
