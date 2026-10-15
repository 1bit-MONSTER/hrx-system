// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/ir/facts_division_test_util.h"

#include <climits>

namespace loom::testing {

uint64_t RawIntegerBits(int64_t value, int32_t bit_count) {
  return (uint64_t)value & iree_math_mask_low_bits_u64(UINT64_MAX, bit_count);
}

int64_t SignedIntegerRepresentation(uint64_t raw_bits, int32_t bit_count) {
  raw_bits &= iree_math_mask_low_bits_u64(UINT64_MAX, bit_count);
  if (bit_count == 1) {
    return (int64_t)raw_bits;
  }
  const uint64_t sign_bit = UINT64_C(1) << (bit_count - 1);
  if ((raw_bits & sign_bit) == 0) {
    return (int64_t)raw_bits;
  }
  const int64_t signed_sign_bit =
      bit_count == 64 ? INT64_MIN : -(int64_t)sign_bit;
  return signed_sign_bit + (int64_t)(raw_bits & (sign_bit - 1));
}

loom_value_facts_t ExactIntegerBits(uint64_t raw_bits, int32_t bit_count) {
  return loom_value_facts_exact_i64(
      SignedIntegerRepresentation(raw_bits, bit_count));
}

bool EvaluateQuotient(loom_value_facts_quotient_kind_t kind, int32_t bit_count,
                      int64_t dividend, int64_t scale, int64_t* out_quotient) {
  const bool shift = kind == LOOM_VALUE_FACTS_QUOTIENT_SHRUI ||
                     kind == LOOM_VALUE_FACTS_QUOTIENT_SHRSI;
  if (shift && (scale < 0 || scale >= bit_count)) {
    return false;
  }
  if (kind == LOOM_VALUE_FACTS_QUOTIENT_DIVUI ||
      kind == LOOM_VALUE_FACTS_QUOTIENT_CEILDIVUI ||
      kind == LOOM_VALUE_FACTS_QUOTIENT_SHRUI) {
    const uint64_t numerator = RawIntegerBits(dividend, bit_count);
    const uint64_t denominator =
        shift ? UINT64_C(1) << scale : RawIntegerBits(scale, bit_count);
    if (denominator == 0) {
      return false;
    }
    const uint64_t quotient = numerator / denominator;
    const uint64_t rounded =
        quotient + (kind == LOOM_VALUE_FACTS_QUOTIENT_CEILDIVUI &&
                    numerator % denominator != 0);
    *out_quotient = SignedIntegerRepresentation(rounded, bit_count);
    return true;
  }
  if (bit_count == 1) {
    dividend = -dividend;
    if (!shift) {
      scale = -scale;
    }
  }
  if (shift) {
    // Arithmetic shift is floor division. C division below handles the other
    // signed cases; an unsigned magnitude handles the 2^63 shift divisor.
    const uint64_t denominator = UINT64_C(1) << scale;
    const uint64_t magnitude = iree_math_magnitude_i64(dividend);
    const uint64_t rounded = magnitude / denominator +
                             (dividend < 0 && magnitude % denominator != 0);
    const int64_t quotient = dividend >= 0 ? (int64_t)rounded
                             : rounded == (UINT64_C(1) << 63)
                                 ? INT64_MIN
                                 : -(int64_t)rounded;
    *out_quotient = bit_count == 1 ? -quotient : quotient;
    return true;
  }
  const int64_t minimum =
      bit_count == 64 ? INT64_MIN : -(INT64_C(1) << (bit_count - 1));
  if (scale == 0 || (dividend == minimum && scale == -1)) {
    return false;
  }
  int64_t quotient = dividend / scale;
  const int64_t remainder = dividend % scale;
  if (remainder != 0) {
    if (kind == LOOM_VALUE_FACTS_QUOTIENT_FLOORDIVSI &&
        (dividend < 0) != (scale < 0)) {
      --quotient;
    } else if (kind == LOOM_VALUE_FACTS_QUOTIENT_CEILDIVSI &&
               (dividend < 0) == (scale < 0)) {
      ++quotient;
    }
  }
  *out_quotient = bit_count == 1 ? -quotient : quotient;
  return true;
}

}  // namespace loom::testing
