// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/analysis/loop_domain.h"

#include <algorithm>
#include <cstdint>
#include <numeric>

#include "iree/testing/gtest.h"
#include "loom/ir/facts.h"
#include "loom/ir/float_facts.h"

namespace loom {
namespace {

TEST(LoopDomainTest, RangeFactsProveNonemptyDomain) {
  const auto lower = loom_value_facts_make(0, 4, 1);
  const auto upper = loom_value_facts_make(8, 16, 1);
  const auto step = loom_value_facts_make(1, 4, 1);
  EXPECT_TRUE(loom_loop_domain_proven_nonempty(lower, upper, step));
  EXPECT_FALSE(loom_loop_domain_proven_empty(lower, upper, step));
}

TEST(LoopDomainTest, RangeFactsProveEmptyDomain) {
  const auto lower = loom_value_facts_make(16, 24, 1);
  const auto upper = loom_value_facts_make(0, 16, 1);
  const auto step = loom_value_facts_exact_i64(1);
  EXPECT_TRUE(loom_loop_domain_proven_empty(lower, upper, step));
  EXPECT_FALSE(loom_loop_domain_proven_nonempty(lower, upper, step));
}

TEST(LoopDomainTest, OverlappingBoundsProveNeitherDomainState) {
  const auto lower = loom_value_facts_make(0, 12, 1);
  const auto upper = loom_value_facts_make(8, 16, 1);
  const auto step = loom_value_facts_exact_i64(1);
  EXPECT_FALSE(loom_loop_domain_proven_empty(lower, upper, step));
  EXPECT_FALSE(loom_loop_domain_proven_nonempty(lower, upper, step));
}

TEST(LoopDomainTest, NonpositiveOrFloatingStepProvesNeitherDomainState) {
  const auto lower = loom_value_facts_exact_i64(0);
  const auto upper = loom_value_facts_exact_i64(16);
  for (const auto step :
       {loom_value_facts_exact_i64(0), loom_value_facts_exact_i64(-1),
        loom_value_facts_unknown(),
        loom_value_facts_exact_float(LOOM_SCALAR_TYPE_F32, 1.0)}) {
    EXPECT_FALSE(loom_loop_domain_proven_empty(lower, upper, step));
    EXPECT_FALSE(loom_loop_domain_proven_nonempty(lower, upper, step));
  }
}

TEST(LoopDomainTest, FloatingBoundsProveNeitherDomainState) {
  const auto floating = loom_value_facts_exact_float(LOOM_SCALAR_TYPE_F32, 0.0);
  const auto lower = loom_value_facts_exact_i64(0);
  const auto upper = loom_value_facts_exact_i64(16);
  const auto step = loom_value_facts_exact_i64(1);
  EXPECT_FALSE(loom_loop_domain_proven_empty(floating, upper, step));
  EXPECT_FALSE(loom_loop_domain_proven_nonempty(floating, upper, step));
  EXPECT_FALSE(loom_loop_domain_proven_empty(lower, floating, step));
  EXPECT_FALSE(loom_loop_domain_proven_nonempty(lower, floating, step));
}

constexpr loom_loop_bound_flags_t kUnsignedExclusive = LOOM_LOOP_BOUND_NONE;
constexpr loom_loop_bound_flags_t kUnsignedInclusive =
    LOOM_LOOP_BOUND_INCLUSIVE;
constexpr loom_loop_bound_flags_t kSignedExclusive = LOOM_LOOP_BOUND_SIGNED;
constexpr loom_loop_bound_flags_t kSignedInclusive =
    LOOM_LOOP_BOUND_SIGNED | LOOM_LOOP_BOUND_INCLUSIVE;

void ExpectCount(loom_loop_bound_flags_t bound_flags, uint8_t bitwidth,
                 uint64_t initial, uint64_t bound, uint64_t step,
                 uint64_t expected) {
  uint64_t actual = UINT64_MAX;
  ASSERT_TRUE(loom_loop_domain_trip_count(bound_flags, bitwidth, initial, bound,
                                          step, &actual));
  EXPECT_EQ(actual, expected);
}

void ExpectUnknown(loom_loop_bound_flags_t bound_flags, uint8_t bitwidth,
                   uint64_t initial, uint64_t bound, uint64_t step) {
  uint64_t actual = UINT64_MAX;
  EXPECT_FALSE(loom_loop_domain_trip_count(bound_flags, bitwidth, initial,
                                           bound, step, &actual));
  EXPECT_EQ(actual, 0u);
}

TEST(LoopDomainTripCountTest, ComparisonSignedness) {
  ExpectCount(kSignedExclusive, 32, -1, 4, 1, 5);
  ExpectCount(kUnsignedExclusive, 32, -1, 4, 1, 0);
  ExpectCount(kSignedInclusive, 32, -1, 4, 1, 6);
  ExpectCount(kUnsignedInclusive, 32, -1, 4, 1, 0);
  ExpectCount(kSignedExclusive, 64, -1, 4, 1, 5);
  ExpectCount(kUnsignedExclusive, 64, -1, 4, 1, 0);
  ExpectCount(kUnsignedExclusive, 32, INT32_MAX, INT32_MIN, 1, 1);
  ExpectCount(kSignedExclusive, 32, INT32_MAX, INT32_MIN, 1, 0);
}

TEST(LoopDomainTripCountTest, NonzeroStartsAndPartialFinalSteps) {
  ExpectCount(kSignedExclusive, 32, 7, 19, 4, 3);
  ExpectCount(kSignedInclusive, 32, 7, 19, 4, 4);
  ExpectCount(kSignedExclusive, 32, 7, 20, 4, 4);
  ExpectCount(kSignedInclusive, 32, 7, 20, 4, 4);
  ExpectCount(kSignedExclusive, 64, -9, 4, 5, 3);
  ExpectCount(kSignedInclusive, 64, -9, -4, 5, 2);
}

TEST(LoopDomainTripCountTest, TerminalIncrementMustFitCarrier) {
  ExpectCount(kSignedExclusive, 32, INT32_MAX - 1, INT32_MAX, 1, 1);
  ExpectUnknown(kSignedExclusive, 32, INT32_MAX - 1, INT32_MAX, 2);
  ExpectUnknown(kSignedInclusive, 32, INT32_MAX, INT32_MAX, 1);
  ExpectCount(kSignedInclusive, 32, INT32_MAX - 1, INT32_MAX - 1, 1, 1);
  ExpectCount(kUnsignedExclusive, 32, UINT32_MAX - 1, UINT32_MAX, 1, 1);
  ExpectUnknown(kUnsignedExclusive, 32, UINT32_MAX - 1, UINT32_MAX, 2);
  ExpectUnknown(kUnsignedInclusive, 32, UINT32_MAX, UINT32_MAX, 1);
  ExpectCount(kSignedExclusive, 64, INT64_MAX - 1, INT64_MAX, 1, 1);
  ExpectUnknown(kSignedExclusive, 64, INT64_MAX - 1, INT64_MAX, 2);
  ExpectUnknown(kSignedInclusive, 64, INT64_MAX, INT64_MAX, 1);
  ExpectCount(kUnsignedExclusive, 64, UINT64_MAX - 1, UINT64_MAX, 1, 1);
  ExpectUnknown(kUnsignedExclusive, 64, UINT64_MAX - 1, UINT64_MAX, 2);
  ExpectUnknown(kUnsignedInclusive, 64, UINT64_MAX, UINT64_MAX, 1);
}

TEST(LoopDomainTripCountTest, FullCarrierSpans) {
  ExpectCount(kSignedExclusive, 64, INT64_MIN, INT64_MAX, 1, UINT64_MAX);
  ExpectCount(kSignedExclusive, 64, INT64_MIN, 0, UINT64_C(1) << 63, 1);
  ExpectCount(kUnsignedExclusive, 64, 0, UINT64_MAX, 1, UINT64_MAX);
  ExpectUnknown(kSignedInclusive, 64, INT64_MIN, INT64_MAX, 1);
  ExpectUnknown(kUnsignedInclusive, 64, 0, UINT64_MAX, 1);
  ExpectUnknown(kUnsignedExclusive, 64, 0, UINT64_MAX, 2);
}

TEST(LoopDomainTripCountTest, EmptyLoopsAndZeroIncrements) {
  ExpectCount(kSignedExclusive, 32, 4, 4, 0, 0);
  ExpectCount(kUnsignedInclusive, 32, 5, 4, -1, 0);
  ExpectUnknown(kSignedExclusive, 32, 3, 4, 0);
  ExpectUnknown(kSignedInclusive, 32, 4, 4, 0);
  ExpectUnknown(kUnsignedExclusive, 32, 0, 4, UINT64_C(1) << 32);
}

TEST(LoopDomainTripCountTest, TruncatesAllInputsToSelectedCarrier) {
  ExpectCount(kSignedExclusive, 32, (UINT64_C(1) << 32) + 3,
              (UINT64_C(1) << 33) + 9, (UINT64_C(1) << 34) + 2, 3);
  ExpectCount(kUnsignedExclusive, 32, UINT64_MAX, 4, 1, 0);
  ExpectCount(kSignedExclusive, 32, UINT32_MAX, 4, 1, 5);
}

void ExpectNumericRange(loom_value_facts_t facts, int64_t lower, int64_t upper,
                        int64_t divisor) {
  EXPECT_EQ(facts.range_lo, lower);
  EXPECT_EQ(facts.range_hi, upper);
  EXPECT_EQ(facts.known_divisor, divisor);
}

TEST(LoopDomainAdditiveRecurrenceTest, PositiveAndNegativeRangeSeeds) {
  loom_loop_recurrence_facts_t facts;
  ASSERT_TRUE(loom_loop_domain_additive_recurrence_facts(
      loom_value_facts_make(0, 256, 256), 64, 4, &facts));
  EXPECT_TRUE(facts.trip_count_known);
  EXPECT_EQ(facts.trip_count, 4u);
  ExpectNumericRange(facts.values, 0, 512, 64);
  ExpectNumericRange(facts.body_values, 0, 448, 64);
  ExpectNumericRange(facts.exit_value, 256, 512, 256);

  ASSERT_TRUE(loom_loop_domain_additive_recurrence_facts(
      loom_value_facts_make(256, 512, 256), -64, 4, &facts));
  ExpectNumericRange(facts.values, 0, 512, 64);
  ExpectNumericRange(facts.body_values, 64, 512, 64);
  ExpectNumericRange(facts.exit_value, 0, 256, 256);
}

TEST(LoopDomainAdditiveRecurrenceTest, ZeroOriginRetainsStrideDivisibility) {
  loom_loop_recurrence_facts_t facts;
  ASSERT_TRUE(loom_loop_domain_additive_recurrence_facts(
      loom_value_facts_exact_i64(0), 128, 8, &facts));
  ExpectNumericRange(facts.values, 0, 1024, 128);
  ExpectNumericRange(facts.body_values, 0, 896, 128);
  ExpectNumericRange(facts.exit_value, 1024, 1024, 1024);
}

TEST(LoopDomainAdditiveRecurrenceTest, OneTripRetainsInitialBodyDivisor) {
  loom_loop_recurrence_facts_t facts;
  ASSERT_TRUE(loom_loop_domain_additive_recurrence_facts(
      loom_value_facts_make(0, 512, 256), 64, 1, &facts));
  ExpectNumericRange(facts.values, 0, 576, 64);
  ExpectNumericRange(facts.body_values, 0, 512, 256);
  ExpectNumericRange(facts.exit_value, 64, 576, 64);

  ASSERT_TRUE(loom_loop_domain_additive_recurrence_facts(
      loom_value_facts_exact_i64(0), -128, 1, &facts));
  ExpectNumericRange(facts.values, -128, 0, 128);
  ExpectNumericRange(facts.body_values, 0, 0, 1);
  ExpectNumericRange(facts.exit_value, -128, -128, 128);
}

TEST(LoopDomainAdditiveRecurrenceTest, ZeroTripsAndStationaryRanges) {
  const auto initial = loom_value_facts_make(128, 256, 128);
  loom_loop_recurrence_facts_t facts;
  for (int64_t step :
       {INT64_MIN, INT64_C(-1), INT64_C(0), INT64_C(1), INT64_MAX}) {
    ASSERT_TRUE(
        loom_loop_domain_additive_recurrence_facts(initial, step, 0, &facts));
    EXPECT_TRUE(facts.trip_count_known);
    EXPECT_EQ(facts.trip_count, 0u);
    ExpectNumericRange(facts.values, 128, 256, 128);
    ExpectNumericRange(facts.exit_value, 128, 256, 128);
    EXPECT_TRUE(loom_value_facts_is_unknown(facts.body_values));
  }
  ASSERT_TRUE(loom_loop_domain_additive_recurrence_facts(initial, 0, UINT64_MAX,
                                                         &facts));
  EXPECT_EQ(facts.trip_count, UINT64_MAX);
  ExpectNumericRange(facts.values, 128, 256, 128);
  ExpectNumericRange(facts.body_values, 128, 256, 128);
  ExpectNumericRange(facts.exit_value, 128, 256, 128);
  ASSERT_TRUE(loom_loop_domain_additive_recurrence_facts(
      loom_value_facts_unknown(), 0, UINT64_MAX, &facts));
  ExpectNumericRange(facts.values, INT64_MIN, INT64_MAX, 1);
}

TEST(LoopDomainAdditiveRecurrenceTest, FullSignedSpan) {
  for (int64_t step : {INT64_C(-1), INT64_C(1)}) {
    const int64_t initial = step > 0 ? INT64_MIN : INT64_MAX;
    const int64_t terminal = step > 0 ? INT64_MAX : INT64_MIN;
    loom_loop_recurrence_facts_t facts;
    ASSERT_TRUE(loom_loop_domain_additive_recurrence_facts(
        loom_value_facts_exact_i64(initial), step, UINT64_MAX, &facts));
    EXPECT_EQ(facts.trip_count, UINT64_MAX);
    ExpectNumericRange(facts.values, INT64_MIN, INT64_MAX, 1);
    ExpectNumericRange(facts.body_values, step > 0 ? INT64_MIN : INT64_MIN + 1,
                       step > 0 ? INT64_MAX - 1 : INT64_MAX, 1);
    ExpectNumericRange(facts.exit_value, terminal, terminal,
                       step > 0 ? INT64_MAX : 1);
  }
}

TEST(LoopDomainAdditiveRecurrenceTest, UnsignedDistanceAndMinimumSignedStep) {
  loom_loop_recurrence_facts_t facts;
  ASSERT_TRUE(loom_loop_domain_additive_recurrence_facts(
      loom_value_facts_exact_i64(INT64_MIN), INT64_MAX, 2, &facts));
  ExpectNumericRange(facts.values, INT64_MIN, INT64_MAX - 1, 1);
  ExpectNumericRange(facts.body_values, INT64_MIN, -1, 1);
  ExpectNumericRange(facts.exit_value, INT64_MAX - 1, INT64_MAX - 1,
                     INT64_MAX - 1);

  ASSERT_TRUE(loom_loop_domain_additive_recurrence_facts(
      loom_value_facts_make(0, INT64_MAX, 1), INT64_MIN, 1, &facts));
  ExpectNumericRange(facts.values, INT64_MIN, INT64_MAX, 1);
  ExpectNumericRange(facts.body_values, 0, INT64_MAX, 1);
  ExpectNumericRange(facts.exit_value, INT64_MIN, -1, 1);

  ASSERT_TRUE(loom_loop_domain_additive_recurrence_facts(
      loom_value_facts_exact_i64(0), INT64_MIN, 1, &facts));
  ExpectNumericRange(facts.values, INT64_MIN, 0, 1);
  ExpectNumericRange(facts.body_values, 0, 0, 1);
  ExpectNumericRange(facts.exit_value, INT64_MIN, INT64_MIN, 1);

  ASSERT_TRUE(loom_loop_domain_additive_recurrence_facts(
      loom_value_facts_exact_i64(INT64_MIN), 128, 2, &facts));
  ExpectNumericRange(facts.values, INT64_MIN, INT64_MIN + 256, 128);
  ExpectNumericRange(facts.body_values, INT64_MIN, INT64_MIN + 128, 128);
  ExpectNumericRange(facts.exit_value, INT64_MIN + 256, INT64_MIN + 256,
                     INT64_MAX - 255);
}

TEST(LoopDomainAdditiveRecurrenceTest, OverflowPublishesNoPartialProof) {
  auto expect_unproven = [](loom_value_facts_t initial, int64_t step,
                            uint64_t count) {
    loom_loop_recurrence_facts_t facts;
    EXPECT_FALSE(loom_loop_domain_additive_recurrence_facts(initial, step,
                                                            count, &facts));
    EXPECT_TRUE(facts.trip_count_known);
    EXPECT_EQ(facts.trip_count, count);
    EXPECT_TRUE(loom_value_facts_is_unknown(facts.values));
    EXPECT_TRUE(loom_value_facts_is_unknown(facts.body_values));
    EXPECT_TRUE(loom_value_facts_is_unknown(facts.exit_value));
  };
  expect_unproven(loom_value_facts_exact_i64(INT64_MAX), 1, 1);
  expect_unproven(loom_value_facts_exact_i64(INT64_MIN), -1, 1);
  expect_unproven(loom_value_facts_exact_i64(INT64_MAX), INT64_MIN, 2);
  expect_unproven(loom_value_facts_make(INT64_MIN, INT64_MIN + 1, 1), 1,
                  UINT64_MAX);
  expect_unproven(loom_value_facts_make(INT64_MAX - 1, INT64_MAX, 1), -1,
                  UINT64_MAX);
}

TEST(LoopDomainAdditiveRecurrenceTest, SmallRangesMatchExplicitIteration) {
  for (int64_t lower = -4; lower <= 4; ++lower) {
    for (int64_t upper = lower; upper <= 4; ++upper) {
      for (int64_t step = -4; step <= 4; ++step) {
        for (uint64_t count = 0; count <= 8; ++count) {
          SCOPED_TRACE(::testing::Message()
                       << "initial=[" << lower << "," << upper
                       << "] step=" << step << " count=" << count);
          int64_t header_lower = INT64_MAX, header_upper = INT64_MIN;
          int64_t body_lower = INT64_MAX, body_upper = INT64_MIN;
          int64_t exit_lower = INT64_MAX, exit_upper = INT64_MIN;
          int64_t header_divisor = 0, body_divisor = 0, exit_divisor = 0;
          for (int64_t initial = lower; initial <= upper; ++initial) {
            int64_t value = initial;
            for (uint64_t iteration = 0; iteration <= count; ++iteration) {
              header_lower = std::min(header_lower, value);
              header_upper = std::max(header_upper, value);
              header_divisor = std::gcd(header_divisor, value);
              if (iteration != count) {
                body_lower = std::min(body_lower, value);
                body_upper = std::max(body_upper, value);
                body_divisor = std::gcd(body_divisor, value);
                value += step;
              }
            }
            exit_lower = std::min(exit_lower, value);
            exit_upper = std::max(exit_upper, value);
            exit_divisor = std::gcd(exit_divisor, value);
          }
          loom_loop_recurrence_facts_t facts;
          ASSERT_TRUE(loom_loop_domain_additive_recurrence_facts(
              loom_value_facts_make(lower, upper, 1), step, count, &facts));
          EXPECT_TRUE(facts.trip_count_known);
          EXPECT_EQ(facts.trip_count, count);
          ExpectNumericRange(facts.values, header_lower, header_upper,
                             std::max(INT64_C(1), header_divisor));
          ExpectNumericRange(facts.exit_value, exit_lower, exit_upper,
                             std::max(INT64_C(1), exit_divisor));
          if (count != 0) {
            ExpectNumericRange(facts.body_values, body_lower, body_upper,
                               std::max(INT64_C(1), body_divisor));
          } else {
            EXPECT_TRUE(loom_value_facts_is_unknown(facts.body_values));
          }
        }
      }
    }
  }
}

TEST(LoopDomainRecurrenceTest, FullSignedCarrierAndTerminalBoundaries) {
  for (const uint8_t bitwidth : {32, 64}) {
    const int64_t maximum = INT64_MAX >> (64 - bitwidth);
    const int64_t minimum = -maximum - 1;
    const auto full = loom_loop_domain_recurrence_facts(
        kSignedExclusive, bitwidth, minimum, maximum, 1);
    EXPECT_TRUE(full.trip_count_known);
    EXPECT_EQ(full.values.range_lo, minimum);
    EXPECT_EQ(full.values.range_hi, maximum);
    EXPECT_EQ(full.body_values.range_lo, minimum);
    EXPECT_EQ(full.body_values.range_hi, maximum - 1);
    EXPECT_EQ(full.exit_value.range_lo, maximum);
    EXPECT_EQ(full.exit_value.range_hi, maximum);
    const auto tail = loom_loop_domain_recurrence_facts(
        kSignedExclusive, bitwidth, maximum - 1, maximum, 2);
    EXPECT_FALSE(tail.trip_count_known);
    EXPECT_TRUE(loom_value_facts_is_unknown(tail.values));
    const auto crossing = loom_loop_domain_recurrence_facts(
        kUnsignedExclusive, bitwidth, maximum, minimum, 1);
    EXPECT_TRUE(crossing.trip_count_known);
    EXPECT_EQ(crossing.trip_count, 1u);
    EXPECT_TRUE(loom_value_facts_is_unknown(crossing.values));
  }
}

TEST(LoopDomainRecurrenceTest, SourceValueMustFitCarrier) {
  const auto facts = loom_loop_domain_recurrence_facts(kSignedExclusive, 32,
                                                       INT64_C(1) << 32, 4, 1);
  EXPECT_TRUE(facts.trip_count_known);
  EXPECT_EQ(facts.trip_count, 4u);
  EXPECT_TRUE(loom_value_facts_is_unknown(facts.values));
}

TEST(LoopDomainRecurrenceTest, ExactExitRetainsUniformity) {
  const auto empty =
      loom_loop_domain_recurrence_facts(kSignedExclusive, 32, 5, 4, 1);
  EXPECT_TRUE(loom_value_facts_is_cluster_uniform(empty.values));
  EXPECT_TRUE(loom_value_facts_is_cluster_uniform(empty.exit_value));
  const auto nonempty =
      loom_loop_domain_recurrence_facts(kSignedExclusive, 32, 0, 4, 1);
  EXPECT_TRUE(loom_value_facts_is_cluster_uniform(nonempty.exit_value));
}

struct ObservedLoop {
  // Whether modular execution reaches a false guard before revisiting a value.
  bool terminates;
  // Whether every executed addition advances in the bound's comparison order.
  bool increases;
  // Number of body executions before exit or recurrence.
  uint64_t trip_count;
  // Whether each increment also advances in the signed source representation.
  bool source_increases;
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

// Small-width interpretation is independent of the closed-form count proof.
// Execute carrier additions and comparisons, stopping when a value repeats.
ObservedLoop InterpretLoop(loom_loop_bound_flags_t bound_flags,
                           uint8_t bitwidth, uint64_t initial, uint64_t bound,
                           uint64_t step) {
  const int64_t modulus = INT64_C(1) << bitwidth;
  const bool is_signed =
      bound_flags == kSignedExclusive || bound_flags == kSignedInclusive;
  auto ordered_value = [&](uint64_t bits) -> int64_t {
    return is_signed && bits >= uint64_t(modulus / 2) ? int64_t(bits) - modulus
                                                      : int64_t(bits);
  };
  auto source_value = [&](uint64_t bits) -> int64_t {
    return bits >= uint64_t(modulus / 2) ? int64_t(bits) - modulus
                                         : int64_t(bits);
  };
  ObservedLoop observed = {true,
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
    bool more =
        bound_flags == kSignedInclusive || bound_flags == kUnsignedInclusive
            ? ordered_value(value) <= ordered_value(bound)
            : ordered_value(value) < ordered_value(bound);
    if (!more) {
      observed.exit_value = source_value(value);
      return observed;
    }
    observed.body_minimum =
        std::min(observed.body_minimum, source_value(value));
    observed.body_maximum =
        std::max(observed.body_maximum, source_value(value));
    uint64_t next = (value + step) % modulus;
    observed.increases &= ordered_value(next) > ordered_value(value);
    observed.source_increases &= source_value(next) > source_value(value);
    observed.minimum = std::min(observed.minimum, source_value(next));
    observed.maximum = std::max(observed.maximum, source_value(next));
    ++observed.trip_count;
    value = next;
  } while (value != initial);
  observed.terminates = false;
  return observed;
}

TEST(LoopDomainTripCountTest, ExhaustiveModularExecution) {
  for (uint8_t bitwidth = 1; bitwidth <= 6; ++bitwidth) {
    const uint64_t limit = UINT64_C(1) << bitwidth;
    for (loom_loop_bound_flags_t bound_flags :
         {kSignedExclusive, kSignedInclusive, kUnsignedExclusive,
          kUnsignedInclusive}) {
      for (uint64_t initial = 0; initial < limit; ++initial) {
        for (uint64_t bound = 0; bound < limit; ++bound) {
          for (uint64_t step = 0; step < limit; ++step) {
            const ObservedLoop expected =
                InterpretLoop(bound_flags, bitwidth, initial, bound, step);
            uint64_t actual = UINT64_MAX;
            const bool exact = loom_loop_domain_trip_count(
                bound_flags, bitwidth, initial, bound, step, &actual);
            const bool expected_exact =
                expected.terminates && expected.increases;
            auto source_value = [&](uint64_t bits) -> int64_t {
              return bits >= limit / 2 ? int64_t(bits) - int64_t(limit)
                                       : int64_t(bits);
            };
            const auto facts = loom_loop_domain_recurrence_facts(
                bound_flags, bitwidth, source_value(initial),
                source_value(bound), source_value(step));
            const bool expected_range =
                expected_exact &&
                (expected.trip_count == 0 ||
                 (source_value(step) > 0 && expected.source_increases));
            if (facts.trip_count_known != exact || facts.trip_count != actual ||
                (expected_range &&
                 (facts.values.range_lo != expected.minimum ||
                  facts.values.range_hi != expected.maximum)) ||
                (!expected_range &&
                 !loom_value_facts_is_unknown(facts.values))) {
              FAIL() << "range width=" << int(bitwidth)
                     << " flags=" << int(bound_flags) << " initial=" << initial
                     << " bound=" << bound << " step=" << step << " actual=["
                     << facts.values.range_lo << "," << facts.values.range_hi
                     << "] expected_range=" << expected_range << " expected=["
                     << expected.minimum << "," << expected.maximum << "]";
            }
            if (exact != expected_exact ||
                actual != (expected_exact ? expected.trip_count : 0)) {
              FAIL() << "width=" << int(bitwidth)
                     << " bound_flags=" << int(bound_flags)
                     << " initial=" << initial << " bound=" << bound
                     << " step=" << step << " exact=" << exact
                     << " count=" << actual
                     << " expected_exact=" << expected_exact
                     << " expected_count=" << expected.trip_count;
            }
            if (expected_range) {
              ASSERT_EQ(facts.exit_value.range_lo, expected.exit_value);
              ASSERT_EQ(facts.exit_value.range_hi, expected.exit_value);
              if (expected.trip_count != 0) {
                ASSERT_EQ(facts.body_values.range_lo, expected.body_minimum);
                ASSERT_EQ(facts.body_values.range_hi, expected.body_maximum);
              } else {
                ASSERT_TRUE(loom_value_facts_is_unknown(facts.body_values));
              }
            } else {
              ASSERT_TRUE(loom_value_facts_is_unknown(facts.body_values));
              ASSERT_TRUE(loom_value_facts_is_unknown(facts.exit_value));
            }
          }
        }
      }
    }
  }
}

}  // namespace
}  // namespace loom
