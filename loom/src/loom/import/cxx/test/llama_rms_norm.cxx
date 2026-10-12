// Copyright (c) 2023-2026 The ggml authors
//
// Licensed under the MIT License. See LICENSE.llama for license information.
// SPDX-License-Identifier: MIT
//
// Standalone specialization; source provenance and adaptations are in
// README.md.

#include <hip/hip_runtime.h>
#include <loomcxx/buffer.h>
#include <loomcxx/check.h>
#include <loomcxx/kernel.h>
#include <loomcxx/scalar.h>
#include <loomcxx/view.h>

__device__ __forceinline__ float warp_sum(float value) {
  for (unsigned offset = 16u; offset > 0u; offset >>= 1u) {
    __builtin_assume(offset < 32u);
    value += __shfl_xor(value, offset, 32);
  }
  return value;
}

static loom::kernel::configuration configure_llama_rms_norm(
    unsigned row_count) {
  return {{row_count, 1, 1}, {64, 1, 1}};
}

[[loom::kernel(configure_llama_rms_norm)]] void llama_rms_norm(
    const float* input, float* output, unsigned ncols, float eps) {
  __shared__ float sums[2];
  unsigned row = blockIdx.x;
  unsigned long long row_offset = (unsigned long long)row * ncols;
  const float* row_input = input + row_offset;
  float* row_output = row_offset + output;
  unsigned tid = threadIdx.x;
  unsigned lane = tid % 32u;
  unsigned warp = tid / 32u;
  float total = 0.0f;
  for (unsigned col = tid; col < ncols; col += 64u) {
    float value = row_input[col];
    total += value * value;
  }
  total = warp_sum(total);
  if (lane == 0u) {
    sums[warp] = total;
  }
  __syncthreads();
  total = 0.0f;
  if (lane < 2u) {
    total = sums[lane];
  }
  total = warp_sum(total);
  float scale = rsqrtf(total / (float)ncols + eps);
  for (unsigned col = tid; col < ncols; col += 64u) {
    row_output[col] = scale * row_input[col];
  }
}

template <unsigned Count, unsigned Columns>
void llama_rms_norm_input(loom::type::buffer<float> input) {
  auto values =
      loom::buffer::view<Count>(input, {}, loom::encoding::layout::dense<1>());
  for (unsigned position = 0; position < Count; ++position) {
    unsigned row = position / Columns;
    unsigned pattern = (position * 37u + row * 11u) % 257u;
    float value = (static_cast<float>(pattern) - 128.0f) / 64.0f;
    loom::view::store(value, values, position);
  }
}

template <unsigned Count, unsigned Columns>
void llama_rms_norm_oracle(loom::type::buffer<float> input,
                           loom::type::buffer<float> output, unsigned ncols,
                           float eps) {
  auto input_values =
      loom::buffer::view<Count>(input, {}, loom::encoding::layout::dense<1>());
  auto output_values =
      loom::buffer::view<Count>(output, {}, loom::encoding::layout::dense<1>());
  for (unsigned row = 0; row < 3u; ++row) {
    float total = 0.0f;
    for (unsigned column = 0; column < Columns; ++column) {
      float value = loom::view::load(input_values, row * Columns + column);
      total += value * value;
    }
    float scale = 1.0f / loom::scalar::sqrtf(total / ncols + eps);
    for (unsigned column = 0; column < Columns; ++column) {
      unsigned position = row * Columns + column;
      float value = loom::view::load(input_values, position);
      loom::view::store(value * scale, output_values, position);
    }
  }
}

LOOM_CHECK_SCENARIO(llama_rms_norm_values) {
  loom::check::trial<1>(
      [](loom::check::ordinal trial, loom::check::entropy entropy) {
        const auto input = loom::check::fill<3, float>(0.0f);
        loom::check::generate<llama_rms_norm_input<3, 1>>(input);
        const auto storage = loom::check::fill<35, float>(-123.0f);
        const auto prefix = loom::check::slice<16>(storage, 0);
        const auto output = loom::check::slice<3>(storage, 16);
        const auto suffix = loom::check::slice<16>(storage, 19);
        loom::check::compare<llama_rms_norm, llama_rms_norm_oracle<3, 1>>(
            loom::kernel::workload(3u), input, output, 1u, 1.0e-5f, [&] {
              loom::check::expect_bitwise(input, input);
              loom::check::expect_close(output, output, 0.0002, 0.0002);
              loom::check::expect_bitwise(prefix, prefix);
              loom::check::expect_bitwise(suffix, suffix);
            });
      });
  loom::check::trial<1>(
      [](loom::check::ordinal trial, loom::check::entropy entropy) {
        const auto input = loom::check::fill<99, float>(0.0f);
        loom::check::generate<llama_rms_norm_input<99, 33>>(input);
        const auto storage = loom::check::fill<131, float>(-123.0f);
        const auto prefix = loom::check::slice<16>(storage, 0);
        const auto output = loom::check::slice<99>(storage, 16);
        const auto suffix = loom::check::slice<16>(storage, 115);
        loom::check::compare<llama_rms_norm, llama_rms_norm_oracle<99, 33>>(
            loom::kernel::workload(3u), input, output, 33u, 1.0e-5f, [&] {
              loom::check::expect_bitwise(input, input);
              loom::check::expect_close(output, output, 0.0002, 0.0002);
              loom::check::expect_bitwise(prefix, prefix);
              loom::check::expect_bitwise(suffix, suffix);
            });
      });
  loom::check::trial<1>(
      [](loom::check::ordinal trial, loom::check::entropy entropy) {
        const auto input = loom::check::fill<387, float>(0.0f);
        loom::check::generate<llama_rms_norm_input<387, 129>>(input);
        const auto storage = loom::check::fill<419, float>(-123.0f);
        const auto prefix = loom::check::slice<16>(storage, 0);
        const auto output = loom::check::slice<387>(storage, 16);
        const auto suffix = loom::check::slice<16>(storage, 403);
        loom::check::compare<llama_rms_norm, llama_rms_norm_oracle<387, 129>>(
            loom::kernel::workload(3u), input, output, 129u, 1.0e-5f, [&] {
              loom::check::expect_bitwise(input, input);
              loom::check::expect_close(output, output, 0.0002, 0.0002);
              loom::check::expect_bitwise(prefix, prefix);
              loom::check::expect_bitwise(suffix, suffix);
            });
      });
}
