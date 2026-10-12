// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "nvfp4_matrix.h"

#include <loomcxx/check.h>
#include <loomcxx/target.h>

static loom::kernel::configuration configure_nvfp4_matrix() {
  return {{1, 1, 1}, {loom::target::subgroup::size(), 1, 1}};
}

[[loom::kernel(configure_nvfp4_matrix)]] void nvfp4_matrix(
    [[loom::noalias, loom::assume_aligned(8)]] const NvFp4Payload* lhs_payload,
    [[loom::noalias, loom::assume_aligned(4)]] const NvFp4Scale* lhs_scale,
    [[loom::noalias, loom::assume_aligned(8)]] const NvFp4Payload* rhs_payload,
    [[loom::noalias, loom::assume_aligned(4)]] const NvFp4Scale* rhs_scale,
    [[loom::noalias, loom::assume_aligned(64)]] float* output) {
  nvfp4_matrix_tile(*lhs_payload, *lhs_scale, *rhs_payload, *rhs_scale, output);
}

LOOM_CHECK_CASE(nvfp4_matrix_ones) {
  // E2M1 code 0x2 and finite E4M3 code 0x38 both represent 1.0.
  const auto lhs_payload = loom::check::fill<2, unsigned>(0x22222222u);
  const auto lhs_scale = loom::check::fill<1, unsigned>(0x38u);
  const auto rhs_payload = loom::check::fill<2, unsigned>(0x22222222u);
  const auto rhs_scale = loom::check::fill<1, unsigned>(0x38u);
  const auto output = loom::check::fill<256, float>(0.0f);
  loom::check::launch<nvfp4_matrix>(lhs_payload, lhs_scale, rhs_payload,
                                    rhs_scale, output);
  loom::check::expect_bitwise(output, loom::check::fill<256, float>(16.0f));
}

LOOM_CHECK_BENCHMARK(nvfp4_matrix_ones_benchmark, nvfp4_matrix_ones);
