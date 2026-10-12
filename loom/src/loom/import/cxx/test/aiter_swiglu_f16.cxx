// Copyright Advanced Micro Devices, Inc. All rights reserved.
//
// Licensed under the MIT License. See LICENSE.aiter for license information.
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

static loom::kernel::configuration configure_aiter_swiglu_f16(
    unsigned token_count) {
  return {{token_count, 1, 1}, {64, 1, 1}};
}

[[loom::kernel(configure_aiter_swiglu_f16)]] void aiter_swiglu_f16(
    const _Float16* input, _Float16* output, unsigned d) {
  unsigned token_idx = blockIdx.x;
  for (unsigned idx = threadIdx.x; idx < d; idx += 64u) {
    float gate = fminf((float)input[token_idx * 2u * d + idx], 7.0f);
    float linear =
        fminf(fmaxf((float)input[token_idx * 2u * d + d + idx], -7.0f), 7.0f);
    float sig = __builtin_amdgcn_rcpf(1.0f + __ocml_exp_f32(-1.702f * gate));
    output[token_idx * d + idx] = (_Float16)(gate * sig * (linear + 1.0f));
  }
}

template <unsigned Count, unsigned Columns>
void aiter_swiglu_f16_input(loom::type::buffer<_Float16> input) {
  auto values =
      loom::buffer::view<Count>(input, {}, loom::encoding::layout::dense<1>());
  for (unsigned position = 0; position < Count; ++position) {
    unsigned row = position / (2u * Columns);
    unsigned half = (position % (2u * Columns)) / Columns;
    unsigned pattern = (position * 73u + row * 29u + half * 41u + 13u) % 257u;
    float value = (static_cast<float>(pattern) - 128.0f) / 8.0f;
    loom::view::store(static_cast<_Float16>(value), values, position);
  }
}

template <unsigned Count, unsigned Columns>
void aiter_swiglu_f16_oracle(loom::type::buffer<_Float16> input,
                             loom::type::buffer<_Float16> output, unsigned d) {
  loom::assume(d == Columns);
  auto input_values =
      loom::buffer::view<Count>(input, {}, loom::encoding::layout::dense<1>());
  auto output_values = loom::buffer::view<Count / 2u>(
      output, {}, loom::encoding::layout::dense<1>());
  for (unsigned token = 0; token < 3u; ++token) {
    for (unsigned column = 0; column < d; ++column) {
      float gate = static_cast<float>(
          loom::view::load(input_values, token * 2u * Columns + column));
      if (7.0f < gate) {
        gate = 7.0f;
      }
      float linear = static_cast<float>(loom::view::load(
          input_values, token * 2u * Columns + Columns + column));
      if (linear < -7.0f) {
        linear = -7.0f;
      }
      if (7.0f < linear) {
        linear = 7.0f;
      }
      float sigmoid = 1.0f / (1.0f + loom::scalar::expf(-1.702f * gate));
      loom::view::store(static_cast<_Float16>(gate * sigmoid * (linear + 1.0f)),
                        output_values, token * Columns + column);
    }
  }
}

LOOM_CHECK_SCENARIO(aiter_swiglu_f16_values) {
  loom::check::trial<1>(
      [](loom::check::ordinal trial, loom::check::entropy entropy) {
        const auto input =
            loom::check::fill<6, _Float16>(static_cast<_Float16>(0.0f));
        loom::check::generate<aiter_swiglu_f16_input<6, 1>>(input);
        const auto storage =
            loom::check::fill<35, _Float16>(static_cast<_Float16>(-123.0f));
        const auto prefix = loom::check::slice<16>(storage, 0);
        const auto output = loom::check::slice<3>(storage, 16);
        const auto suffix = loom::check::slice<16>(storage, 19);
        loom::check::compare<aiter_swiglu_f16, aiter_swiglu_f16_oracle<6, 1>>(
            loom::kernel::workload(3u), input, output, 1u, [&] {
              loom::check::expect_bitwise(input, input);
              loom::check::expect_close(output, output, 0.002, 0.002);
              loom::check::expect_bitwise(prefix, prefix);
              loom::check::expect_bitwise(suffix, suffix);
            });
      });
  loom::check::trial<1>([](loom::check::ordinal trial,
                           loom::check::entropy entropy) {
    const auto input =
        loom::check::fill<186, _Float16>(static_cast<_Float16>(0.0f));
    loom::check::generate<aiter_swiglu_f16_input<186, 31>>(input);
    const auto storage =
        loom::check::fill<125, _Float16>(static_cast<_Float16>(-123.0f));
    const auto prefix = loom::check::slice<16>(storage, 0);
    const auto output = loom::check::slice<93>(storage, 16);
    const auto suffix = loom::check::slice<16>(storage, 109);
    loom::check::compare<aiter_swiglu_f16, aiter_swiglu_f16_oracle<186, 31>>(
        loom::kernel::workload(3u), input, output, 31u, [&] {
          loom::check::expect_bitwise(input, input);
          loom::check::expect_close(output, output, 0.002, 0.002);
          loom::check::expect_bitwise(prefix, prefix);
          loom::check::expect_bitwise(suffix, suffix);
        });
  });
  loom::check::trial<1>([](loom::check::ordinal trial,
                           loom::check::entropy entropy) {
    const auto input =
        loom::check::fill<390, _Float16>(static_cast<_Float16>(0.0f));
    loom::check::generate<aiter_swiglu_f16_input<390, 65>>(input);
    const auto storage =
        loom::check::fill<227, _Float16>(static_cast<_Float16>(-123.0f));
    const auto prefix = loom::check::slice<16>(storage, 0);
    const auto output = loom::check::slice<195>(storage, 16);
    const auto suffix = loom::check::slice<16>(storage, 211);
    loom::check::compare<aiter_swiglu_f16, aiter_swiglu_f16_oracle<390, 65>>(
        loom::kernel::workload(3u), input, output, 65u, [&] {
          loom::check::expect_bitwise(input, input);
          loom::check::expect_close(output, output, 0.002, 0.002);
          loom::check::expect_bitwise(prefix, prefix);
          loom::check::expect_bitwise(suffix, suffix);
        });
  });
  loom::check::trial<1>([](loom::check::ordinal trial,
                           loom::check::entropy entropy) {
    const auto input =
        loom::check::fill<774, _Float16>(static_cast<_Float16>(0.0f));
    loom::check::generate<aiter_swiglu_f16_input<774, 129>>(input);
    const auto storage =
        loom::check::fill<419, _Float16>(static_cast<_Float16>(-123.0f));
    const auto prefix = loom::check::slice<16>(storage, 0);
    const auto output = loom::check::slice<387>(storage, 16);
    const auto suffix = loom::check::slice<16>(storage, 403);
    loom::check::compare<aiter_swiglu_f16, aiter_swiglu_f16_oracle<774, 129>>(
        loom::kernel::workload(3u), input, output, 129u, [&] {
          loom::check::expect_bitwise(input, input);
          loom::check::expect_close(output, output, 0.002, 0.002);
          loom::check::expect_bitwise(prefix, prefix);
          loom::check::expect_bitwise(suffix, suffix);
        });
  });
}
