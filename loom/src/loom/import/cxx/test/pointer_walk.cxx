// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <hip/hip_runtime.h>
#include <loomcxx/check.h>
#include <loomcxx/view.h>

__device__ __forceinline__ const int* advance(const int* pointer,
                                              long long count) {
  if (count == 0) {
    return pointer;
  }
  return +(pointer + count);
}

__global__ [[loom::workgroup_size(64, 1, 1), loom::workgroup_count(1, 1, 1)]]
void pointer_walk(const int* input, const int* counts, int* output,
                  unsigned length, long long start, long long displacement) {
  unsigned lane = threadIdx.x;
  if (lane < length) {
    unsigned count = (unsigned)counts[lane];
    const int* base = advance(input, start + (long long)lane * 64);
    const int* selected = (lane & 1u) ? base + 2 : 4 + base;
    int* result = output + (unsigned long long)lane * 10;
    result[0] = selected[-1];
    result[1] = *advance(selected, displacement);
    result[2] = *(&2 [base]);
    const int* cursor = base;
    int total = 0;
    for (unsigned index = 0; index < count; ++index) {
      total += *advance(base, index);
      ++cursor;
    }
    result[3] = total;
    result[4] = *cursor;
    unsigned index = 0;
    total = 0;
    while (index < count) {
      --cursor;
      total += *cursor;
      ++index;
    }
    result[5] = total;
    result[6] = *cursor;
    index = 0;
    do {
      cursor += 1;
      ++index;
    } while (index < count);
    result[7] = *cursor;
    cursor -= 1;
    result[8] = *cursor;
    (*(result + 9)) = *(cursor - 1);
  }
}

void pointer_walk_input(loom::type::buffer<int> input) {
  auto values =
      loom::buffer::view<512>(input, {}, loom::encoding::layout::dense<1>());
  for (unsigned position = 0; position < 512u; ++position) {
    loom::view::store((int)((position * 97u + 13u) % 257u) - 128, values,
                      position);
  }
}

void pointer_walk_counts(loom::type::buffer<int> counts) {
  auto values =
      loom::buffer::view<7>(counts, {}, loom::encoding::layout::dense<1>());
  loom::view::store(0, values, 0);
  loom::view::store(1, values, 1);
  loom::view::store(2, values, 2);
  loom::view::store(5, values, 3);
  loom::view::store(17, values, 4);
  loom::view::store(33, values, 5);
  loom::view::store(47, values, 6);
}

long long pointer_walk_start(loom::check::ordinal trial) {
  unsigned trial_value = trial;
  return trial_value == 0u ? 1ll : (trial_value == 1u ? 7ll : 31ll);
}

long long pointer_walk_displacement(loom::check::ordinal trial) {
  unsigned trial_value = trial;
  return trial_value == 0u ? -1ll : (trial_value == 1u ? -2ll : 3ll);
}

int pointer_walk_load(loom::type::buffer<int> input, long long position) {
  loom::assume(position >= 0ll && position < 512ll);
  auto values =
      loom::buffer::view<512>(input, {}, loom::encoding::layout::dense<1>());
  return loom::view::load(values, (unsigned)position);
}

void pointer_walk_oracle(loom::type::buffer<int> input,
                         loom::type::buffer<int> counts,
                         loom::type::buffer<int> output, unsigned length,
                         long long start, long long displacement) {
  loom::assume(length <= 7u);
  auto count_values =
      loom::buffer::view<7>(counts, {}, loom::encoding::layout::dense<1>());
  auto output_values =
      loom::buffer::view<7, 10>(output, {}, loom::encoding::layout::dense<2>());
  for (unsigned lane = 0; lane < length; ++lane) {
    loom::assume(lane < 7u);
    unsigned count = (unsigned)loom::view::load(count_values, lane);
    loom::assume(count <= 47u);
    long long base = start + (long long)lane * 64ll;
    long long selected = base + ((lane & 1u) ? 2ll : 4ll);
    int total = 0;
    for (unsigned index = 0; index < count; ++index) {
      total += pointer_walk_load(input, base + (long long)index);
    }
    unsigned trips = count == 0u ? 1u : count;
    loom::view::store(pointer_walk_load(input, selected - 1ll), output_values,
                      lane, 0);
    loom::view::store(pointer_walk_load(input, selected + displacement),
                      output_values, lane, 1);
    loom::view::store(pointer_walk_load(input, base + 2ll), output_values, lane,
                      2);
    loom::view::store(total, output_values, lane, 3);
    loom::view::store(pointer_walk_load(input, base + (long long)count),
                      output_values, lane, 4);
    loom::view::store(total, output_values, lane, 5);
    loom::view::store(pointer_walk_load(input, base), output_values, lane, 6);
    loom::view::store(pointer_walk_load(input, base + (long long)trips),
                      output_values, lane, 7);
    loom::view::store(pointer_walk_load(input, base + (long long)trips - 1ll),
                      output_values, lane, 8);
    loom::view::store(pointer_walk_load(input, base + (long long)trips - 2ll),
                      output_values, lane, 9);
  }
}

LOOM_CHECK_SCENARIO(pointer_walk_values) {
  loom::check::trial<3>(
      [](loom::check::ordinal trial, loom::check::entropy entropy) {
        const auto input = loom::check::fill<int, 512>(0);
        loom::check::generate<pointer_walk_input>(input);
        const auto counts = loom::check::fill<int, 7>(0);
        loom::check::generate<pointer_walk_counts>(counts);
        const auto start = loom::check::generate<pointer_walk_start>(trial);
        const auto displacement =
            loom::check::generate<pointer_walk_displacement>(trial);
        const auto storage = loom::check::fill<int, 102>(-123);
        const auto output = loom::check::slice<70>(storage, 16);
        loom::check::compare<pointer_walk, pointer_walk_oracle>(
            input, counts, output, 7u, start, displacement, [&] {
              loom::check::expect_bitwise(input, input);
              loom::check::expect_bitwise(counts, counts);
              loom::check::expect_bitwise(storage, storage);
            });
      });
}
