// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <loomcxx/check.h>

static float increment(float value) { return value + 0.125f; }
static double identity(double value) { return value; }
static float from_bits(unsigned bits) {
  return __builtin_bit_cast(float, bits);
}

LOOM_CHECK_CASE(absolute_error) {
  const auto actual = increment(1.0f);
  loom::check::expect_close(actual, 1.0f, 1.0 / 8.0, 0.0);
  loom::check::expect_close(actual, 1.0f, 0.125, 0.0, "different");
}

LOOM_CHECK_CASE(relative_error) {
  const auto actual = identity(2.125);
  loom::check::expect_close(actual, 2.0, 0.0, 1.0 / 16.0);
}

LOOM_CHECK_CASE(exceptional_values) {
  const auto actual_nan = from_bits(0x7fc00001u);
  const auto expected_nan = from_bits(0x7fc00002u);
  const auto infinity = from_bits(0x7f800000u);
  loom::check::expect_close(actual_nan, expected_nan, 0.0, 0.0, "same");
  loom::check::expect_close(infinity, infinity, 0.0, 0.0, "different");
}
