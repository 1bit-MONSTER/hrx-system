// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/analysis/loop_domain.h"

#include "loom/ir/facts.h"

static bool loom_loop_domain_has_positive_step(loom_value_facts_t lower_bound,
                                               loom_value_facts_t upper_bound,
                                               loom_value_facts_t step) {
  return !loom_value_facts_is_float(lower_bound) &&
         !loom_value_facts_is_float(upper_bound) &&
         !loom_value_facts_is_float(step) && loom_value_facts_is_positive(step);
}

bool loom_loop_domain_proven_empty(loom_value_facts_t lower_bound,
                                   loom_value_facts_t upper_bound,
                                   loom_value_facts_t step) {
  return loom_loop_domain_has_positive_step(lower_bound, upper_bound, step) &&
         lower_bound.range_lo >= upper_bound.range_hi;
}

bool loom_loop_domain_proven_nonempty(loom_value_facts_t lower_bound,
                                      loom_value_facts_t upper_bound,
                                      loom_value_facts_t step) {
  return loom_loop_domain_has_positive_step(lower_bound, upper_bound, step) &&
         lower_bound.range_hi < upper_bound.range_lo;
}

bool loom_loop_domain_trip_count(loom_loop_bound_flags_t bound_flags,
                                 uint8_t bitwidth, uint64_t initial_value,
                                 uint64_t upper_bound, uint64_t step,
                                 uint64_t* out_trip_count) {
  *out_trip_count = 0;
  const bool is_signed = iree_any_bit_set(bound_flags, LOOM_LOOP_BOUND_SIGNED);
  const bool is_inclusive =
      iree_any_bit_set(bound_flags, LOOM_LOOP_BOUND_INCLUSIVE);

  // Flipping the sign bit maps signed order to unsigned order. Modular addition
  // is unchanged by this rotation, so both comparison domains share one proof.
  const uint64_t mask = UINT64_MAX >> (64 - bitwidth);
  const uint64_t sign_bit = is_signed ? UINT64_C(1) << (bitwidth - 1) : 0;
  const uint64_t initial = (initial_value & mask) ^ sign_bit;
  const uint64_t upper = (upper_bound & mask) ^ sign_bit;
  const uint64_t increment = step & mask;
  if (initial > upper || (initial == upper && !is_inclusive)) {
    return true;
  }
  if (increment == 0 || (is_inclusive && upper == mask)) {
    return false;
  }

  const uint64_t distance = upper - initial + (is_inclusive ? 1 : 0);
  const uint64_t trip_count = (distance - 1) / increment + 1;
  if (trip_count > (mask - initial) / increment) {
    return false;
  }
  *out_trip_count = trip_count;
  return true;
}

static int64_t loom_loop_domain_value_from_rank(uint64_t rank) {
  const uint64_t sign_bit = UINT64_C(1) << 63;
  return rank >= sign_bit ? (int64_t)(rank - sign_bit)
                          : INT64_MIN + (int64_t)rank;
}

bool loom_loop_domain_additive_recurrence_facts(
    loom_value_facts_t initial_values, int64_t step, uint64_t trip_count,
    loom_loop_recurrence_facts_t* out_facts) {
  *out_facts = (loom_loop_recurrence_facts_t){
      .values = loom_value_facts_unknown(),
      .body_values = loom_value_facts_unknown(),
      .exit_value = loom_value_facts_unknown(),
      .trip_count = trip_count,
      .trip_count_known = true,
  };
  initial_values =
      loom_value_facts_make(initial_values.range_lo, initial_values.range_hi,
                            initial_values.known_divisor);
  if (trip_count == 0 || step == 0) {
    out_facts->values = initial_values;
    out_facts->exit_value = initial_values;
    if (trip_count != 0) {
      out_facts->body_values = initial_values;
    }
    return true;
  }

  // Signed-order ranks cover the full i64 span without signed subtraction or
  // a signed trip-count product. Checking the advancing endpoint proves that
  // every initial value and intermediate state remain representable.
  const uint64_t sign_bit = UINT64_C(1) << 63;
  const uint64_t initial_lo_rank = (uint64_t)initial_values.range_lo ^ sign_bit;
  const uint64_t initial_hi_rank = (uint64_t)initial_values.range_hi ^ sign_bit;
  const uint64_t magnitude = iree_math_magnitude_i64(step);
  const uint64_t available =
      step > 0 ? UINT64_MAX - initial_hi_rank : initial_lo_rank;
  if (trip_count > available / magnitude) {
    return false;
  }
  const uint64_t distance = trip_count * magnitude;
  const uint64_t exit_lo_rank =
      step > 0 ? initial_lo_rank + distance : initial_lo_rank - distance;
  const uint64_t exit_hi_rank =
      step > 0 ? initial_hi_rank + distance : initial_hi_rank - distance;
  const int64_t exit_lo = loom_loop_domain_value_from_rank(exit_lo_rank);
  const int64_t exit_hi = loom_loop_domain_value_from_rank(exit_hi_rank);
  const uint64_t initial_divisor =
      loom_value_facts_is_exact(initial_values)
          ? iree_math_magnitude_i64(initial_values.range_lo)
          : (uint64_t)initial_values.known_divisor;
  uint64_t divisor = iree_math_gcd_u64(initial_divisor, magnitude);
  // The fact representation cannot store the magnitude of INT64_MIN as its
  // positive signed divisor, matching the exact-integer constructor.
  if (divisor > (uint64_t)INT64_MAX) {
    divisor = 1;
  }
  out_facts->values = loom_value_facts_make(
      step > 0 ? initial_values.range_lo : exit_lo,
      step > 0 ? exit_hi : initial_values.range_hi, (int64_t)divisor);
  out_facts->body_values = loom_value_facts_make(
      step > 0 ? initial_values.range_lo
               : loom_loop_domain_value_from_rank(exit_lo_rank + magnitude),
      step > 0 ? loom_loop_domain_value_from_rank(exit_hi_rank - magnitude)
               : initial_values.range_hi,
      trip_count == 1 ? initial_values.known_divisor : (int64_t)divisor);
  out_facts->exit_value = loom_value_facts_make(
      exit_lo, exit_hi,
      (int64_t)iree_math_gcd_u64((uint64_t)initial_values.known_divisor,
                                 distance));
  return true;
}

loom_loop_recurrence_facts_t loom_loop_domain_recurrence_facts(
    loom_loop_bound_flags_t bound_flags, uint8_t bitwidth,
    int64_t initial_value, int64_t upper_bound, int64_t step) {
  loom_loop_recurrence_facts_t result = {
      .values = loom_value_facts_unknown(),
      .body_values = loom_value_facts_unknown(),
      .exit_value = loom_value_facts_unknown(),
  };
  result.trip_count_known = loom_loop_domain_trip_count(
      bound_flags, bitwidth, (uint64_t)initial_value, (uint64_t)upper_bound,
      (uint64_t)step, &result.trip_count);
  const int64_t maximum = INT64_MAX >> (64 - bitwidth);
  const int64_t minimum = -maximum - 1;
  if (!result.trip_count_known || initial_value < minimum ||
      initial_value > maximum) {
    return result;
  }
  if (result.trip_count == 0) {
    result.values = loom_value_facts_exact_i64(initial_value);
    result.exit_value = result.values;
    return result;
  }
  if (step <= 0 || step > maximum) {
    return result;
  }
  if (loom_loop_domain_additive_recurrence_facts(
          loom_value_facts_exact_i64(initial_value), step, result.trip_count,
          &result) &&
      loom_value_facts_fit_signed_bit_count(result.values, bitwidth)) {
    result.exit_value = loom_value_facts_exact_i64(result.exit_value.range_lo);
  } else {
    result.values = loom_value_facts_unknown();
    result.body_values = loom_value_facts_unknown();
    result.exit_value = loom_value_facts_unknown();
  }
  return result;
}
