// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <hip/hip_runtime.h>
#include <loomcxx/check.h>
#include <loomcxx/view.h>

__device__ __forceinline__ void store(int* output, unsigned index, int value) {
  output[index] = value;
}

__device__ __forceinline__ bool next_condition(int* output, unsigned index,
                                               unsigned count) {
  output[index] = output[index] + 1;
  return (unsigned)output[index] < count;
}

// Each lane independently exercises zero-trip, post-test and nested loops.
__global__
    [[loom::workgroup_size(64, 1, 1), loom::workgroup_count(1, 1, 1)]] void
    control_flow(const int* counts, int* output, unsigned length) {
  unsigned lane = threadIdx.x;
  if (lane < length) {
    unsigned count = (unsigned)counts[lane];
    unsigned base = lane * 10u;
    unsigned index = 0;
    int total = 0;
    while (index < count) {
      total += (int)index + 1;
      ++index;
    }
    store(output, base, total);
    store(output, base + 1u, (int)index);

    index = 0;
    total = 0;
    do {
      total += (int)index + 1;
      ++index;
    } while (index < count);
    store(output, base + 2u, total);
    store(output, base + 3u, (int)index);

    index = 0;
    total = 0;
    while (index < count) {
      unsigned inner = 0;
      do {
        total += (int)(index + inner + 1u);
        ++inner;
      } while (inner < (count & 3u));
      ++index;
    }
    store(output, base + 4u, total);

    for (index = 0; index < count;) {
      ++index;
    }
    store(output, base + 5u, (int)index);

    store(output, base + 6u, 0);
    total = 0;
    while (next_condition(output, base + 6u, count)) {
      ++total;
    }
    store(output, base + 7u, total);

    store(output, base + 8u, 0);
    total = 0;
    do {
      ++total;
    } while (next_condition(output, base + 8u, count));
    store(output, base + 9u, total);
  }
}

void control_flow_input(loom::type::buffer<int> input) {
  auto values =
      loom::buffer::view<7>(input, {}, loom::encoding::layout::dense<1>());
  loom::view::store(0, values, 0);
  loom::view::store(1, values, 1);
  loom::view::store(2, values, 2);
  loom::view::store(7, values, 3);
  loom::view::store(31, values, 4);
  loom::view::store(64, values, 5);
  loom::view::store(129, values, 6);
}

void control_flow_oracle(loom::type::buffer<int> counts,
                         loom::type::buffer<int> output, unsigned length) {
  loom::assume(length <= 7u);
  auto input_values =
      loom::buffer::view<7>(counts, {}, loom::encoding::layout::dense<1>());
  auto output_values =
      loom::buffer::view<7, 10>(output, {}, loom::encoding::layout::dense<2>());
  for (unsigned lane = 0; lane < length; ++lane) {
    loom::assume(lane < 7u);
    unsigned count = (unsigned)loom::view::load(input_values, lane);
    unsigned trips = count == 0u ? 1u : count;
    unsigned total = count * (count + 1u) / 2u;
    unsigned post_total = trips * (trips + 1u) / 2u;
    unsigned inner_count = count & 3u;
    if (inner_count == 0u) {
      inner_count = 1u;
    }
    unsigned nested_total = 0u;
    for (unsigned outer = 0; outer < count; ++outer) {
      for (unsigned inner = 0; inner < inner_count; ++inner) {
        nested_total += outer + inner + 1u;
      }
    }
    loom::view::store((int)total, output_values, lane, 0);
    loom::view::store((int)count, output_values, lane, 1);
    loom::view::store((int)post_total, output_values, lane, 2);
    loom::view::store((int)trips, output_values, lane, 3);
    loom::view::store((int)nested_total, output_values, lane, 4);
    loom::view::store((int)count, output_values, lane, 5);
    loom::view::store((int)trips, output_values, lane, 6);
    loom::view::store((int)(count == 0u ? 0u : count - 1u), output_values, lane,
                      7);
    loom::view::store((int)trips, output_values, lane, 8);
    loom::view::store((int)trips, output_values, lane, 9);
  }
}

LOOM_CHECK_SCENARIO(control_flow_values) {
  loom::check::trial<1>(
      [](loom::check::ordinal trial, loom::check::entropy entropy) {
        const auto counts = loom::check::fill<int, 7>(0);
        loom::check::generate<control_flow_input>(counts);
        const auto storage = loom::check::fill<int, 102>(-123);
        const auto output = loom::check::slice<70>(storage, 16);
        loom::check::compare<control_flow, control_flow_oracle>(
            counts, output, 7u, [&] {
              loom::check::expect_bitwise(counts, counts);
              loom::check::expect_bitwise(storage, storage);
            });
      });
}
