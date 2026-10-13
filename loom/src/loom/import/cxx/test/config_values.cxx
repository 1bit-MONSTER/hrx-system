// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <loomcxx/check.h>
#include <loomcxx/index.h>

[[loom::config("tuning.factor")]] extern const loom::type::index factor;
[[loom::config("tuning.count")]] extern const unsigned count;
[[loom::config("tuning.enabled")]] extern const bool enabled;

unsigned configured_sum(unsigned start) {
  unsigned result = start;
  [[loom::unroll(factor)]]
  for (unsigned index = 0; index < count; ++index) {
    result += enabled ? index * factor : index;
  }
  return result;
}

LOOM_CHECK_CASE(five) {
  const auto actual = configured_sum(3u);
  // 3 + 5 * (0 + 1 + 2 + 3 + 4 + 5 + 6).
  loom::check::expect_equal(actual, 108u);
}
LOOM_CHECK_BENCHMARK(five_benchmark, five);

LOOM_CHECK_CASE(nine) {
  const auto actual = configured_sum(3u);
  // Disabled scaling selects 3 + (0 + ... + 12).
  loom::check::expect_equal(actual, 81u);
}
LOOM_CHECK_BENCHMARK(nine_benchmark, nine);
