// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <loomcxx/check.h>
#include <loomcxx/kernel.h>

static loom::kernel::configuration configure_copy(unsigned item_count) {
  unsigned width = loom::target::subgroup_size();
  return {{(item_count + width - 1) / width, 1, 1}, {width, 1, 1}};
}

[[loom::kernel(configure_copy)]] void configured_copy(unsigned item_count,
                                                      const float* input,
                                                      float* output) {
  unsigned item =
      loom::workgroup_id.x * loom::workgroup_size.x + loom::workitem_id.x;
  if (item < item_count) {
    output[item] = input[item];
  }
}

LOOM_CHECK_CASE(configured_copy_case) {
  const auto input = loom::check::iota<float, 67>(-1.0f, 0.25f, 8);
  const auto output = loom::check::fill<float, 67>(0.0f);
  loom::check::launch<configured_copy>(loom::kernel::workload(67u), 67u, input,
                                       output);
  loom::check::expect_bitwise(output, input);
}
