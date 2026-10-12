// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <hip/hip_runtime.h>
#include <loomcxx/check.h>
#include <loomcxx/view.h>

#define PIXELS 320u

__forceinline__ unsigned counted_stride(unsigned start) {
  unsigned total = 0;
  [[loom::unroll(3)]]
  for (unsigned index = start; index < (PIXELS / 16u); index += (1u << 2)) {
    total += index;
  }
  return total;
}

__forceinline__ unsigned counted_edge(unsigned start) {
  unsigned total = 0;
  [[loom::unroll(2)]]
  for (unsigned index = start; index < (~0u - 3u); index += (1u << 2)) {
    total += index;
  }
  return total;
}

__forceinline__ unsigned counted_empty(unsigned start) {
  unsigned total = 7;
  [[loom::unroll(2)]]
  for (unsigned index = start; index < (4u - 4u); index += (1u << 2)) {
    total += index;
  }
  return total;
}

__forceinline__ unsigned counted_maximum_step(unsigned start) {
  unsigned total = 0;
  [[loom::unroll(2)]]
  for (unsigned index = start; index < (4u / 4u); index += ~0u) {
    total += index + 7u;
  }
  return total;
}

template <unsigned Width, unsigned Step, unsigned Unroll, unsigned Pipeline>
__forceinline__ unsigned sum(const unsigned* input, unsigned start) {
  constexpr unsigned lanes = 16;
  unsigned total = 0;
  [[loom::unroll(Unroll), loom::pipeline(Pipeline)]]
  for (unsigned index = start; index < (Width / lanes); index += Step / 2u) {
    total += input[index];
  }
  return total;
}

__forceinline__ unsigned sum_wrapped(const unsigned* input, unsigned start) {
  unsigned total = 0;
  [[loom::unroll(3), loom::pipeline(2)]]
  for (unsigned index = start; index < (0xffffffffu + 21u);
       index += (1u << 2)) {
    total += input[index];
  }
  return total;
}

__forceinline__ unsigned sum_sized(const unsigned* input, unsigned start) {
  unsigned total = 0;
  [[loom::unroll(3), loom::pipeline(2)]]
  for (unsigned index = start;
       index < static_cast<unsigned>(sizeof(++start) * 5u); index++) {
    total += input[index];
  }
  return total + start;
}

__forceinline__ unsigned sum_mutable(const unsigned* input, unsigned start) {
  unsigned bound = 20;
  unsigned total = 0;
  for (unsigned index = start; index < --bound; ++index) {
    total += input[index];
  }
  return total + bound;
}

__global__ [[loom::workgroup_size(64, 1, 1), loom::workgroup_count(1, 1, 1)]]
void constant_loops(const unsigned* input, unsigned* output, unsigned start) {
  unsigned lane = threadIdx.x;
  const unsigned* row = input + lane * 20u;
  output[lane * 12u] = sum<PIXELS, 4, 1, 1>(row, start);
  output[lane * 12u + 1u] = sum<PIXELS, 4, 3, 1>(row, start);
  output[lane * 12u + 2u] = sum<PIXELS, 4, 1, 2>(row, 0u);
  output[lane * 12u + 3u] = sum<PIXELS, 4, 3, 2>(row, 0u);
  output[lane * 12u + 4u] = sum_wrapped(row, 0u);
  output[lane * 12u + 5u] = sum_sized(row, 0u);
  output[lane * 12u + 6u] = sum_mutable(row, start);
  output[lane * 12u + 7u] = counted_edge(0xfffffff0u + (lane & 7u));
  output[lane * 12u + 8u] = sum<0, 4, 3, 2>(row, 0u);
  output[lane * 12u + 9u] = sum<16, 4, 3, 2>(row, 0u);
  output[lane * 12u + 10u] = sum<32, 4, 3, 2>(row, 0u);
  output[lane * 12u + 11u] = sum<80, 4, 3, 2>(row, 0u);
}

unsigned constant_loop_start(loom::check::ordinal trial) {
  unsigned trial_value = trial;
  if (trial_value < 4u) {
    return trial_value;
  }
  if (trial_value == 4u) {
    return 7u;
  }
  if (trial_value < 10u) {
    return 11u + trial_value;
  }
  if (trial_value == 10u) {
    return 21u;
  }
  if (trial_value == 11u) {
    return 0x80000000u;
  }
  return 0xffffffffu;
}

unsigned constant_edge_start(loom::check::ordinal trial) {
  return 0xfffffff0u + (unsigned)trial;
}

unsigned counted_stride_reference(unsigned start) {
  if (start >= 20u) {
    return 0u;
  }
  unsigned iterations = (20u - start + 3u) / 4u;
  return iterations * start + iterations * (iterations - 1u) * 2u;
}

unsigned counted_edge_reference(unsigned start) {
  unsigned bound = 0xfffffffcu;
  if (start >= bound) {
    return 0u;
  }
  unsigned iterations = (bound - start + 3u) / 4u;
  return iterations * start + iterations * (iterations - 1u) * 2u;
}

void constant_loop_function_observations(unsigned start,
                                         loom::type::buffer<unsigned> output) {
  auto values =
      loom::buffer::view<3>(output, {}, loom::encoding::layout::dense<1>());
  loom::view::store(counted_stride(start), values, 0);
  loom::view::store(counted_empty(start), values, 1);
  loom::view::store(counted_maximum_step(start), values, 2);
}

void constant_loop_function_oracle(unsigned start,
                                   loom::type::buffer<unsigned> output) {
  auto values =
      loom::buffer::view<3>(output, {}, loom::encoding::layout::dense<1>());
  loom::view::store(counted_stride_reference(start), values, 0);
  loom::view::store(7u, values, 1);
  loom::view::store(start == 0u ? 7u : 0u, values, 2);
}

void constant_edge_observation(unsigned start,
                               loom::type::buffer<unsigned> output) {
  auto values =
      loom::buffer::view<1>(output, {}, loom::encoding::layout::dense<1>());
  loom::view::store(counted_edge(start), values, 0);
}

void constant_edge_oracle(unsigned start, loom::type::buffer<unsigned> output) {
  auto values =
      loom::buffer::view<1>(output, {}, loom::encoding::layout::dense<1>());
  loom::view::store(counted_edge_reference(start), values, 0);
}

void constant_loop_input(loom::type::buffer<unsigned> input) {
  auto values =
      loom::buffer::view<1280>(input, {}, loom::encoding::layout::dense<1>());
  for (unsigned position = 0; position < 1280u; ++position) {
    loom::view::store((position * 17u + 7u) % 251u, values, position);
  }
}

unsigned constant_row_sum(loom::type::buffer<unsigned> input, unsigned lane,
                          unsigned begin, unsigned end, unsigned step) {
  loom::assume(lane < 64u && begin <= end && end <= 20u && step > 0u &&
               step <= 4u);
  auto values =
      loom::buffer::view<64, 20>(input, {}, loom::encoding::layout::dense<2>());
  unsigned total = 0u;
  for (unsigned column = begin; column < end; column += step) {
    loom::assume(column < 20u);
    total += loom::view::load(values, lane, column);
  }
  return total;
}

void constant_loops_oracle(loom::type::buffer<unsigned> input,
                           loom::type::buffer<unsigned> output,
                           unsigned start) {
  auto output_values = loom::buffer::view<64, 12>(
      output, {}, loom::encoding::layout::dense<2>());
  for (unsigned lane = 0; lane < 64u; ++lane) {
    unsigned start_sum =
        start < 20u ? constant_row_sum(input, lane, start, 20u, 2u) : 0u;
    unsigned even_sum = constant_row_sum(input, lane, 0u, 20u, 2u);
    unsigned wrapped_sum = constant_row_sum(input, lane, 0u, 20u, 4u);
    unsigned all_sum = constant_row_sum(input, lane, 0u, 20u, 1u);
    unsigned mutable_sum = 0u;
    unsigned mutable_bound = 19u;
    if (start < 20u) {
      unsigned iterations = (20u - start) / 2u;
      mutable_sum =
          constant_row_sum(input, lane, start, start + iterations, 1u);
      mutable_bound -= iterations;
    }
    unsigned bound1_sum = constant_row_sum(input, lane, 0u, 1u, 2u);
    unsigned bound2_sum = constant_row_sum(input, lane, 0u, 2u, 2u);
    unsigned bound5_sum = constant_row_sum(input, lane, 0u, 5u, 2u);
    loom::view::store(start_sum, output_values, lane, 0);
    loom::view::store(start_sum, output_values, lane, 1);
    loom::view::store(even_sum, output_values, lane, 2);
    loom::view::store(even_sum, output_values, lane, 3);
    loom::view::store(wrapped_sum, output_values, lane, 4);
    loom::view::store(all_sum, output_values, lane, 5);
    loom::view::store(mutable_sum + mutable_bound, output_values, lane, 6);
    loom::view::store(counted_edge_reference(0xfffffff0u + (lane & 7u)),
                      output_values, lane, 7);
    loom::view::store(0u, output_values, lane, 8);
    loom::view::store(bound1_sum, output_values, lane, 9);
    loom::view::store(bound2_sum, output_values, lane, 10);
    loom::view::store(bound5_sum, output_values, lane, 11);
  }
}

LOOM_CHECK_SCENARIO(constant_loops_functions) {
  loom::check::trial<13>([](loom::check::ordinal trial,
                            loom::check::entropy entropy) {
    const auto start = loom::check::generate<constant_loop_start>(trial);
    const auto output = loom::check::fill<3, unsigned>(-123u);
    loom::check::compare<constant_loop_function_observations,
                         constant_loop_function_oracle>(
        start, output, [&] { loom::check::expect_bitwise(output, output); });
  });
  loom::check::trial<16>([](loom::check::ordinal trial,
                            loom::check::entropy entropy) {
    const auto start = loom::check::generate<constant_edge_start>(trial);
    const auto output = loom::check::fill<1, unsigned>(-123u);
    loom::check::compare<constant_edge_observation, constant_edge_oracle>(
        start, output, [&] { loom::check::expect_bitwise(output, output); });
  });
}

LOOM_CHECK_SCENARIO(constant_loop_cases) {
  loom::check::trial<13>(
      [](loom::check::ordinal trial, loom::check::entropy entropy) {
        const auto start = loom::check::generate<constant_loop_start>(trial);
        const auto input = loom::check::fill<1280, unsigned>(0u);
        loom::check::generate<constant_loop_input>(input);
        const auto storage = loom::check::fill<800, unsigned>(-123u);
        const auto output = loom::check::slice<768>(storage, 16);
        loom::check::compare<constant_loops, constant_loops_oracle>(
            input, output, start, [&] {
              loom::check::expect_bitwise(input, input);
              loom::check::expect_bitwise(storage, storage);
            });
      });
}
