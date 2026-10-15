// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <cstdint>

#include "iree/testing/gtest.h"
#include "loom/analysis/loop_domain.h"
#include "loom/analysis/loop_domain_test_util.h"

namespace loom {
namespace {

constexpr loom_loop_bound_flags_t kComparisonFlags[] = {
    LOOM_LOOP_BOUND_NONE,
    LOOM_LOOP_BOUND_INCLUSIVE,
    LOOM_LOOP_BOUND_SIGNED,
    LOOM_LOOP_BOUND_SIGNED | LOOM_LOOP_BOUND_INCLUSIVE,
    LOOM_LOOP_BOUND_DESCENDING,
    LOOM_LOOP_BOUND_INCLUSIVE | LOOM_LOOP_BOUND_DESCENDING,
    LOOM_LOOP_BOUND_SIGNED | LOOM_LOOP_BOUND_DESCENDING,
    LOOM_LOOP_BOUND_SIGNED | LOOM_LOOP_BOUND_INCLUSIVE |
        LOOM_LOOP_BOUND_DESCENDING,
};

constexpr uint8_t kStressCarrierBitwidth = 6;

TEST(LoopDomainStressTest, ExhaustiveNextGuardExecution) {
  for (uint8_t bitwidth = 1; bitwidth <= kStressCarrierBitwidth; ++bitwidth) {
    const uint64_t limit = UINT64_C(1) << bitwidth;
    for (auto flags : kComparisonFlags) {
      for (uint64_t initial = 0; initial < limit; ++initial) {
        for (uint64_t bound = 0; bound < limit; ++bound) {
          for (uint64_t step = 0; step < limit; ++step) {
            ASSERT_TRUE(loom::testing::CheckNextGuardLoopDomainExecution(
                flags, bitwidth, initial, bound, step));
          }
        }
      }
    }
  }
}

TEST(LoopDomainStressTest, ExhaustiveModularExecution) {
  for (uint8_t bitwidth = 1; bitwidth <= kStressCarrierBitwidth; ++bitwidth) {
    const uint64_t limit = UINT64_C(1) << bitwidth;
    for (auto flags : kComparisonFlags) {
      for (uint64_t initial = 0; initial < limit; ++initial) {
        for (uint64_t bound = 0; bound < limit; ++bound) {
          for (uint64_t step = 0; step < limit; ++step) {
            ASSERT_TRUE(loom::testing::CheckLoopDomainExecution(
                flags, bitwidth, initial, bound, step));
          }
        }
      }
    }
  }
}

}  // namespace
}  // namespace loom
