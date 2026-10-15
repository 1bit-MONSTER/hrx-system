// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/analysis/loop_domain_test_util.h"

#include <algorithm>

#include "loom/ir/facts.h"

namespace loom::testing {

LoopDomainExecution InterpretLoopDomainExecution(
    loom_loop_bound_flags_t bound_flags, uint8_t bitwidth, uint64_t initial,
    uint64_t bound, uint64_t step, bool guard_uses_next) {
  const int64_t modulus = INT64_C(1) << bitwidth;
  const bool is_signed = bound_flags & LOOM_LOOP_BOUND_SIGNED;
  const bool inclusive = bound_flags & LOOM_LOOP_BOUND_INCLUSIVE;
  const bool descending = bound_flags & LOOM_LOOP_BOUND_DESCENDING;
  auto ordered_value = [&](uint64_t bits) -> int64_t {
    return is_signed && bits >= uint64_t(modulus / 2) ? int64_t(bits) - modulus
                                                      : int64_t(bits);
  };
  auto source_value = [&](uint64_t bits) -> int64_t {
    return bits >= uint64_t(modulus / 2) ? int64_t(bits) - modulus
                                         : int64_t(bits);
  };
  LoopDomainExecution observed = {true,
                                  true,
                                  0,
                                  true,
                                  source_value(initial),
                                  source_value(initial),
                                  INT64_MAX,
                                  INT64_MIN,
                                  0};
  uint64_t value = initial;
  do {
    const uint64_t guard_value =
        guard_uses_next ? (value + step) % modulus : value;
    const int64_t current = ordered_value(guard_value);
    const int64_t limit = ordered_value(bound);
    const bool more = (descending ? current > limit : current < limit) ||
                      (inclusive && current == limit);
    if (!more) {
      observed.exit_value = source_value(value);
      return observed;
    }
    observed.body_minimum =
        std::min(observed.body_minimum, source_value(value));
    observed.body_maximum =
        std::max(observed.body_maximum, source_value(value));
    const uint64_t next = (value + step) % modulus;
    const uint64_t next_guard =
        guard_uses_next ? (next + step) % modulus : next;
    observed.advances &= descending ? ordered_value(next_guard) < current
                                    : ordered_value(next_guard) > current;
    const int64_t source_step = source_value(step);
    observed.source_translations_fit &=
        source_step < 0 ? source_value(next) < source_value(value)
                        : source_value(next) > source_value(value);
    observed.minimum = std::min(observed.minimum, source_value(next));
    observed.maximum = std::max(observed.maximum, source_value(next));
    ++observed.trip_count;
    value = next;
  } while (value != initial);
  observed.terminates = false;
  return observed;
}

::testing::AssertionResult CheckNextGuardLoopDomainExecution(
    loom_loop_bound_flags_t bound_flags, uint8_t bitwidth, uint64_t initial,
    uint64_t bound, uint64_t step) {
  const uint64_t limit = UINT64_C(1) << bitwidth;
  auto source_value = [&](uint64_t bits) -> int64_t {
    return bits >= limit / 2 ? int64_t(bits) - int64_t(limit) : int64_t(bits);
  };
  const LoopDomainExecution expected = InterpretLoopDomainExecution(
      bound_flags, bitwidth, initial, bound, step, true);
  const auto facts = loom_loop_domain_next_guard_recurrence_facts(
      bound_flags, bitwidth, loom_value_facts_exact_i64(source_value(initial)),
      loom_value_facts_exact_i64(source_value(bound)),
      loom_value_facts_exact_i64(source_value(step)));
  const bool expected_exact = expected.terminates && expected.advances;
  const bool expected_range =
      expected_exact &&
      (expected.trip_count == 0 || expected.source_translations_fit);
  if ((facts.trip_count_kind == LOOM_LOOP_TRIP_COUNT_EXACT) != expected_exact ||
      facts.trip_count != (expected_exact ? expected.trip_count : 0) ||
      (expected_range && (facts.values.range_lo != expected.minimum ||
                          facts.values.range_hi != expected.maximum ||
                          facts.exit_value.range_lo != expected.exit_value ||
                          facts.exit_value.range_hi != expected.exit_value)) ||
      (!expected_range && (!loom_value_facts_is_unknown(facts.values) ||
                           !loom_value_facts_is_unknown(facts.body_values) ||
                           !loom_value_facts_is_unknown(facts.exit_value))) ||
      (expected_range && expected.trip_count != 0 &&
       (facts.body_values.range_lo != expected.body_minimum ||
        facts.body_values.range_hi != expected.body_maximum))) {
    return ::testing::AssertionFailure()
           << "width=" << int(bitwidth) << " flags=" << int(bound_flags)
           << " initial=" << initial << " bound=" << bound << " step=" << step
           << " actual_count=" << facts.trip_count
           << " expected_count=" << expected.trip_count << " actual=["
           << facts.values.range_lo << "," << facts.values.range_hi
           << "] expected=[" << expected.minimum << "," << expected.maximum
           << "]";
  }
  return ::testing::AssertionSuccess();
}

::testing::AssertionResult CheckLoopDomainExecution(
    loom_loop_bound_flags_t bound_flags, uint8_t bitwidth, uint64_t initial,
    uint64_t bound, uint64_t step) {
  const uint64_t limit = UINT64_C(1) << bitwidth;
  auto source_value = [&](uint64_t bits) -> int64_t {
    return bits >= limit / 2 ? int64_t(bits) - int64_t(limit) : int64_t(bits);
  };
  const LoopDomainExecution expected =
      InterpretLoopDomainExecution(bound_flags, bitwidth, initial, bound, step);
  uint64_t actual = UINT64_MAX;
  const bool exact = loom_loop_domain_trip_count(bound_flags, bitwidth, initial,
                                                 bound, step, &actual);
  const bool expected_exact = expected.terminates && expected.advances;
  const auto facts = loom_loop_domain_recurrence_facts(
      bound_flags, bitwidth, loom_value_facts_exact_i64(source_value(initial)),
      loom_value_facts_exact_i64(source_value(bound)),
      loom_value_facts_exact_i64(source_value(step)));
  const bool expected_range =
      expected_exact &&
      (expected.trip_count == 0 ||
       ((bound_flags & LOOM_LOOP_BOUND_DESCENDING ? source_value(step) < 0
                                                  : source_value(step) > 0) &&
        expected.source_translations_fit));
  const bool range_matches =
      expected_range && facts.values.range_lo == expected.minimum &&
      facts.values.range_hi == expected.maximum &&
      facts.exit_value.range_lo == expected.exit_value &&
      facts.exit_value.range_hi == expected.exit_value &&
      (expected.trip_count == 0
           ? loom_value_facts_is_unknown(facts.body_values)
           : facts.body_values.range_lo == expected.body_minimum &&
                 facts.body_values.range_hi == expected.body_maximum);
  const bool unknown_matches = !expected_range &&
                               loom_value_facts_is_unknown(facts.values) &&
                               loom_value_facts_is_unknown(facts.body_values) &&
                               loom_value_facts_is_unknown(facts.exit_value);
  if ((facts.trip_count_kind == LOOM_LOOP_TRIP_COUNT_EXACT) != exact ||
      facts.trip_count != actual ||
      (expected_range ? !range_matches : !unknown_matches) ||
      exact != expected_exact ||
      actual != (expected_exact ? expected.trip_count : 0)) {
    return ::testing::AssertionFailure()
           << "width=" << int(bitwidth) << " flags=" << int(bound_flags)
           << " initial=" << initial << " bound=" << bound << " step=" << step
           << " exact=" << exact << " count=" << actual
           << " expected_exact=" << expected_exact
           << " expected_count=" << expected.trip_count << " actual=["
           << facts.values.range_lo << "," << facts.values.range_hi
           << "] expected_range=" << expected_range << " expected=["
           << expected.minimum << "," << expected.maximum << "]";
  }
  return ::testing::AssertionSuccess();
}

}  // namespace loom::testing
