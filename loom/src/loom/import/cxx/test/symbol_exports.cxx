// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <loomcxx/check.h>
#include <loomcxx/view.h>

namespace arithmetic {
[[loom::symbol("arithmetic.mix")]] static unsigned mix(unsigned value);
static unsigned mix(unsigned value) { return value * value + 7u; }
}  // namespace arithmetic

[[loom::kernel, loom::symbol("library.dispatch"), loom::workgroup_size(1, 1, 1),
  loom::workgroup_count(1, 1, 1)]]
void entry(unsigned* output, const unsigned* input) {
  output[0u] = arithmetic::mix(input[0u]);
}

void symbol_export_input(loom::type::buffer<unsigned> input,
                         loom::check::ordinal trial,
                         unsigned long long random_word) {
  auto values =
      loom::buffer::view<1>(input, {}, loom::encoding::layout::dense<1>());
  unsigned trial_value = trial;
  unsigned value = (unsigned)random_word;
  if (trial_value == 0u) {
    value = 0u;
  }
  if (trial_value == 1u) {
    value = 1u;
  }
  if (trial_value == 2u) {
    value = 2u;
  }
  if (trial_value == 3u) {
    value = 3u;
  }
  if (trial_value == 4u) {
    value = 0xffu;
  }
  if (trial_value == 5u) {
    value = 0x100u;
  }
  if (trial_value == 6u) {
    value = 0xffffu;
  }
  if (trial_value == 7u) {
    value = 0x10000u;
  }
  if (trial_value == 8u) {
    value = 0x7fffffffu;
  }
  if (trial_value == 9u) {
    value = 0x80000000u;
  }
  if (trial_value == 10u) {
    value = 0xfffffffeu;
  }
  if (trial_value == 11u) {
    value = 0xffffffffu;
  }
  loom::view::store(value, values, 0);
}

void symbol_export_oracle(loom::type::buffer<unsigned> output,
                          loom::type::buffer<unsigned> input) {
  auto input_values =
      loom::buffer::view<1>(input, {}, loom::encoding::layout::dense<1>());
  auto output_values =
      loom::buffer::view<1>(output, {}, loom::encoding::layout::dense<1>());
  unsigned value = loom::view::load(input_values, 0);
  loom::view::store(value * value + 7u, output_values, 0);
}

LOOM_CHECK_SCENARIO(symbol_export_values) {
  loom::check::trial<32>(
      [](loom::check::ordinal trial, loom::check::entropy entropy) {
        const auto input = loom::check::fill<1, unsigned>(0u);
        const auto stream = loom::check::fork(entropy, "input");
        const auto random_word = loom::check::read(stream, trial);
        loom::check::generate<symbol_export_input>(input, trial, random_word);
        const auto storage = loom::check::fill<33, unsigned>(-123u);
        const auto output = loom::check::slice<1>(storage, 16);
        loom::check::compare<entry, symbol_export_oracle>(output, input, [&] {
          loom::check::expect_bitwise(input, input);
          loom::check::expect_bitwise(storage, storage);
        });
      });
}
