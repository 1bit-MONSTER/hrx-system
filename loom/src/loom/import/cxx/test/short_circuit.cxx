// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <hip/hip_runtime.h>
#include <loomcxx/check.h>
#include <loomcxx/view.h>

__device__ __forceinline__ int record(int* output, unsigned index, int digit,
                                      int value) {
  output[index] = output[index] * 10 + digit;
  return value;
}

// Lanes beyond length must skip the input load and its effectful consumer.
__global__
    [[loom::workgroup_size(64, 1, 1), loom::workgroup_count(1, 1, 1)]] void
    short_circuit(const int* input, int* output, unsigned length) {
  unsigned lane = threadIdx.x;
  unsigned base = lane * 12u;
  output[base] = 0;
  output[base + 1u] = lane < length && record(output, base, 1, input[lane]);
  output[base + 2u] = 0;
  output[base + 3u] =
      lane >= length || record(output, base + 2u, 2, input[lane]);
  output[base + 4u] = 0;
  output[base + 5u] = record(output, base + 4u, 1, (int)(lane & 1u)) &&
                      record(output, base + 4u, 2, (int)(lane & 2u));
  output[base + 6u] = 0;
  output[base + 7u] = record(output, base + 6u, 1, (int)(lane & 1u)) ||
                      record(output, base + 6u, 2, (int)(lane & 2u));
  output[base + 8u] = 0;
  ((lane & 8u) && record(output, base + 8u, 4, 1));
  output[base + 9u] = (float(lane) - 31.0f) && (int(lane) - 32);
  // Each successful bound records its digit before evaluating the next bound.
  output[base + 10u] = 0;
  output[base + 11u] = lane < 64u && record(output, base + 10u, 1, 1) &&
                       lane < 32u && record(output, base + 10u, 2, 1) &&
                       lane < 16u && record(output, base + 10u, 3, 1) &&
                       lane < 8u && record(output, base + 10u, 4, 1) &&
                       lane < 4u && record(output, base + 10u, 5, 1) &&
                       lane < 2u && record(output, base + 10u, 6, 1) &&
                       lane < 1u;
}

template <unsigned Count>
void short_circuit_input(loom::type::buffer<int> input) {
  auto values =
      loom::buffer::view<Count>(input, {}, loom::encoding::layout::dense<1>());
  for (unsigned position = 0; position < Count; ++position) {
    loom::view::store((int)((position * 7u) % 9u) - 3, values, position);
  }
}

void short_circuit_input_1(loom::type::buffer<int> input) {
  short_circuit_input<1>(input);
}

void short_circuit_input_17(loom::type::buffer<int> input) {
  short_circuit_input<17>(input);
}

void short_circuit_input_33(loom::type::buffer<int> input) {
  short_circuit_input<33>(input);
}

void short_circuit_input_64(loom::type::buffer<int> input) {
  short_circuit_input<64>(input);
}

int short_circuit_value(unsigned position) {
  return (int)((position * 7u) % 9u) - 3;
}

int short_circuit_trace(unsigned lane) {
  if (lane < 2u) {
    return 123456;
  }
  if (lane < 4u) {
    return 12345;
  }
  if (lane < 8u) {
    return 1234;
  }
  if (lane < 16u) {
    return 123;
  }
  if (lane < 32u) {
    return 12;
  }
  return 1;
}

void short_circuit_oracle(loom::type::buffer<int> input,
                          loom::type::buffer<int> output, unsigned length) {
  static_cast<void>(input);
  auto output_values =
      loom::buffer::view<768>(output, {}, loom::encoding::layout::dense<1>());
  for (unsigned lane = 0; lane < 64u; ++lane) {
    unsigned base = lane * 12u;
    bool present = lane < length;
    bool truth = present && short_circuit_value(lane) != 0;
    bool first = (lane & 1u) != 0u;
    bool second = (lane & 2u) != 0u;
    loom::view::store((int)present, output_values, base);
    loom::view::store((int)truth, output_values, base + 1u);
    loom::view::store(present ? 2 : 0, output_values, base + 2u);
    loom::view::store((int)(!present || truth), output_values, base + 3u);
    loom::view::store(first ? 12 : 1, output_values, base + 4u);
    loom::view::store((int)(first && second), output_values, base + 5u);
    loom::view::store(first ? 1 : 12, output_values, base + 6u);
    loom::view::store((int)(first || second), output_values, base + 7u);
    loom::view::store((lane & 8u) != 0u ? 4 : 0, output_values, base + 8u);
    loom::view::store((int)(lane != 31u && lane != 32u), output_values,
                      base + 9u);
    loom::view::store(short_circuit_trace(lane), output_values, base + 10u);
    loom::view::store((int)(lane == 0u), output_values, base + 11u);
  }
}

LOOM_CHECK_SCENARIO(short_circuit_values) {
  loom::check::trial<1>(
      [](loom::check::ordinal trial, loom::check::entropy entropy) {
        const auto input = loom::check::fill<int, 1>(0);
        loom::check::generate<short_circuit_input_1>(input);
        const auto storage = loom::check::fill<int, 800>(-123);
        const auto output = loom::check::slice<768>(storage, 16);
        loom::check::compare<short_circuit, short_circuit_oracle>(
            input, output, 0u, [&] {
              loom::check::expect_bitwise(input, input);
              loom::check::expect_bitwise(storage, storage);
            });
      });
  loom::check::trial<1>(
      [](loom::check::ordinal trial, loom::check::entropy entropy) {
        const auto input = loom::check::fill<int, 1>(0);
        loom::check::generate<short_circuit_input_1>(input);
        const auto storage = loom::check::fill<int, 800>(-123);
        const auto output = loom::check::slice<768>(storage, 16);
        loom::check::compare<short_circuit, short_circuit_oracle>(
            input, output, 1u, [&] {
              loom::check::expect_bitwise(input, input);
              loom::check::expect_bitwise(storage, storage);
            });
      });
  loom::check::trial<1>(
      [](loom::check::ordinal trial, loom::check::entropy entropy) {
        const auto input = loom::check::fill<int, 17>(0);
        loom::check::generate<short_circuit_input_17>(input);
        const auto storage = loom::check::fill<int, 800>(-123);
        const auto output = loom::check::slice<768>(storage, 16);
        loom::check::compare<short_circuit, short_circuit_oracle>(
            input, output, 17u, [&] {
              loom::check::expect_bitwise(input, input);
              loom::check::expect_bitwise(storage, storage);
            });
      });
  loom::check::trial<1>(
      [](loom::check::ordinal trial, loom::check::entropy entropy) {
        const auto input = loom::check::fill<int, 33>(0);
        loom::check::generate<short_circuit_input_33>(input);
        const auto storage = loom::check::fill<int, 800>(-123);
        const auto output = loom::check::slice<768>(storage, 16);
        loom::check::compare<short_circuit, short_circuit_oracle>(
            input, output, 33u, [&] {
              loom::check::expect_bitwise(input, input);
              loom::check::expect_bitwise(storage, storage);
            });
      });
  loom::check::trial<1>(
      [](loom::check::ordinal trial, loom::check::entropy entropy) {
        const auto input = loom::check::fill<int, 64>(0);
        loom::check::generate<short_circuit_input_64>(input);
        const auto storage = loom::check::fill<int, 800>(-123);
        const auto output = loom::check::slice<768>(storage, 16);
        loom::check::compare<short_circuit, short_circuit_oracle>(
            input, output, 64u, [&] {
              loom::check::expect_bitwise(input, input);
              loom::check::expect_bitwise(storage, storage);
            });
      });
}
