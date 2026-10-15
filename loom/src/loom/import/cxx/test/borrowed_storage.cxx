// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <loomcxx/check.h>
#include <loomcxx/kernel.h>
#include <loomcxx/view.h>

[[loom::kernel, loom::workgroup_size(32, 1, 1), loom::workgroup_count(2, 1, 1)]]
void borrowed_storage(const unsigned* input_pointer,
                      loom::type::buffer<const unsigned> input_buffer,
                      unsigned* output) {
  unsigned item =
      loom::kernel::workgroup::id.x * 32u + loom::kernel::workitem::id.x;
  auto* interior =
      loom::buffer::assume::memory_space<loom::memory_space::constant>(
          input_pointer + 3);
  auto buffer =
      loom::buffer::assume::memory_space<loom::memory_space::constant>(
          input_buffer);
  auto source =
      loom::buffer::view<70>(buffer, {}, loom::encoding::layout::dense<1>());
  output[item * 2 + 1] = interior[item];
  output[item * 2 + 2] = loom::view::load(source, item + 3);
}

void borrowed_storage_oracle(loom::type::buffer<unsigned> first_input,
                             loom::type::buffer<unsigned> second_input,
                             loom::type::buffer<unsigned> output) {
  auto first = loom::buffer::view<70>(first_input, {},
                                      loom::encoding::layout::dense<1>());
  auto second = loom::buffer::view<70>(second_input, {},
                                       loom::encoding::layout::dense<1>());
  auto destination =
      loom::buffer::view<130>(output, {}, loom::encoding::layout::dense<1>());
  for (unsigned item = 0; item < 64; ++item) {
    loom::view::store(loom::view::load(first, item + 3), destination,
                      item * 2 + 1);
    loom::view::store(loom::view::load(second, item + 3), destination,
                      item * 2 + 2);
  }
}

LOOM_CHECK_SCENARIO(borrowed_storage_values) {
  loom::check::trial<1>(
      [](loom::check::ordinal trial, loom::check::entropy entropy) {
        const auto input = loom::check::iota<70, unsigned>(0xffffffc0u, 7u, 23);
        const auto output = loom::check::fill<130, unsigned>(37u);
        // Both borrowed forms alias the same immutable input. The oracle uses
        // ordinary views and also checks the untouched input and output guards.
        loom::check::compare<borrowed_storage, borrowed_storage_oracle>(
            input, input, output, [&] {
              loom::check::expect_bitwise(input, input);
              loom::check::expect_bitwise(output, output);
            });
      });
}
