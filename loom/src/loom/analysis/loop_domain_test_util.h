// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_ANALYSIS_LOOP_DOMAIN_TEST_UTIL_H_
#define LOOM_ANALYSIS_LOOP_DOMAIN_TEST_UTIL_H_

#include <cstdint>

#include "iree/testing/gtest.h"
#include "loom/analysis/loop_domain.h"

namespace loom::testing {

// Concrete modular execution observed independently of the closed-form loop
// domain analysis.
struct LoopDomainExecution {
  // Whether execution reaches a false guard before revisiting a value.
  bool terminates;
  // Whether every executed addition advances in the selected direction.
  bool advances;
  // Number of body executions before exit or recurrence.
  uint64_t trip_count;
  // Whether each increment stays monotonic in its signed source direction.
  bool source_translations_fit;
  // Smallest signed source value observed at the header, including its exit.
  int64_t minimum;
  // Largest signed source value observed at the header, including its exit.
  int64_t maximum;
  // Smallest signed source value observed during an executed body.
  int64_t body_minimum;
  // Largest signed source value observed during an executed body.
  int64_t body_maximum;
  // Signed source value at the first false guard, if execution terminates.
  int64_t exit_value;
};

// Interprets one modular loop, stopping when its guard fails or a value
// repeats. Inputs are carrier bit patterns that fit within bitwidth.
LoopDomainExecution InterpretLoopDomainExecution(
    loom_loop_bound_flags_t bound_flags, uint8_t bitwidth, uint64_t initial,
    uint64_t bound, uint64_t step, bool guard_uses_next = false);

// Compares trip-count and recurrence facts with independent modular execution.
::testing::AssertionResult CheckLoopDomainExecution(
    loom_loop_bound_flags_t bound_flags, uint8_t bitwidth, uint64_t initial,
    uint64_t bound, uint64_t step);

// Compares next-guard recurrence facts with independent modular execution.
::testing::AssertionResult CheckNextGuardLoopDomainExecution(
    loom_loop_bound_flags_t bound_flags, uint8_t bitwidth, uint64_t initial,
    uint64_t bound, uint64_t step);

}  // namespace loom::testing

#endif  // LOOM_ANALYSIS_LOOP_DOMAIN_TEST_UTIL_H_
