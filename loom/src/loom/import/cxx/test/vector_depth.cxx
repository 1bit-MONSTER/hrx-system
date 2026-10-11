// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <loomcxx/check.h>
#include <loomcxx/kernel.h>
#include <loomcxx/view.h>

// Packed framebuffer update: retain the low color bits while replacing depth.
typedef unsigned u32x16 __attribute__((vector_size(64)));

static u32x16 depth16(u32x16 previous, u32x16 depth) {
  return (previous & 65535u) | (depth & 0xffff0000u);
}

[[loom::kernel, loom::workgroup_size(1, 1, 1), loom::workgroup_count(1, 1, 1)]]
void vector_depth(const unsigned* previous_words, const unsigned* depth_words,
                  unsigned* output_words, unsigned count) {
  const u32x16* previous = reinterpret_cast<const u32x16*>(previous_words);
  const u32x16* depth = reinterpret_cast<const u32x16*>(depth_words);
  u32x16* output = reinterpret_cast<u32x16*>(output_words);
  for (unsigned block = 0; block < count; ++block) {
    output[block] = depth16(previous[block], depth[block]);
  }
}

// The caller supplies 64-byte-aligned spans. Whole vectors form the body;
// the scalar tail preserves both the color bits and the exact access extent.
[[loom::kernel, loom::workgroup_size(1, 1, 1), loom::workgroup_count(1, 1, 1)]]
void vector_depth_span(const unsigned* previous, unsigned* output,
                       unsigned count, unsigned depth, unsigned step) {
  const u32x16 lanes = {0u, 1u, 2u,  3u,  4u,  5u,  6u,  7u,
                        8u, 9u, 10u, 11u, 12u, 13u, 14u, 15u};
  const u32x16* input_vectors = reinterpret_cast<const u32x16*>(previous);
  u32x16* output_vectors = reinterpret_cast<u32x16*>(output);
  unsigned blocks = count / 16u;
  for (unsigned block = 0; block < blocks; ++block) {
    u32x16 depths = lanes * step + depth;
    output_vectors[block] = depth16(input_vectors[block], depths);
    depth += 16u * step;
  }
  for (unsigned pixel = blocks * 16u; pixel < count; ++pixel) {
    output[pixel] = (previous[pixel] & 65535u) | (depth & 0xffff0000u);
    depth += step;
  }
}

unsigned vector_depth_input_value(unsigned position, unsigned salt) {
  if (position == 0u) {
    return salt == 0u ? 0u : 0xffffffffu;
  }
  if (position == 1u) {
    return salt == 0u ? 0xffffffffu : 0u;
  }
  if (position == 2u) {
    return salt == 0u ? 0x12345678u : 0x87654321u;
  }
  if (position == 3u) {
    return salt == 0u ? 0xffff0000u : 0x0000ffffu;
  }
  unsigned value = position * 0x9e3779b9u + salt * 0x85ebca6bu;
  return value ^ (value >> 13u);
}

template <unsigned Count>
void vector_depth_inputs(loom::type::buffer<unsigned> previous,
                         loom::type::buffer<unsigned> depth) {
  auto previous_values = loom::buffer::view<Count>(
      previous, {}, loom::encoding::layout::dense<1>());
  auto depth_values =
      loom::buffer::view<Count>(depth, {}, loom::encoding::layout::dense<1>());
  for (unsigned position = 0; position < Count; ++position) {
    loom::view::store(vector_depth_input_value(position, 0u), previous_values,
                      position);
    loom::view::store(vector_depth_input_value(position, 1u), depth_values,
                      position);
  }
}

unsigned vector_depth_expected(unsigned previous, unsigned depth) {
  return (previous & 0xffffu) | (depth & 0xffff0000u);
}

template <unsigned Count>
void vector_depth_oracle(loom::type::buffer<unsigned> previous,
                         loom::type::buffer<unsigned> depth,
                         loom::type::buffer<unsigned> output,
                         unsigned block_count) {
  static_cast<void>(block_count);
  auto previous_values = loom::buffer::view<Count>(
      previous, {}, loom::encoding::layout::dense<1>());
  auto depth_values =
      loom::buffer::view<Count>(depth, {}, loom::encoding::layout::dense<1>());
  auto output_values =
      loom::buffer::view<Count>(output, {}, loom::encoding::layout::dense<1>());
  for (unsigned position = 0; position < Count; ++position) {
    loom::view::store(
        vector_depth_expected(loom::view::load(previous_values, position),
                              loom::view::load(depth_values, position)),
        output_values, position);
  }
}

template <unsigned Count>
void vector_depth_span_input(loom::type::buffer<unsigned> previous) {
  auto values = loom::buffer::view<Count>(previous, {},
                                          loom::encoding::layout::dense<1>());
  for (unsigned position = 0; position < Count; ++position) {
    unsigned value = position * 0x7f4a7c15u + 0x6d2b79f5u;
    loom::view::store(value ^ (value >> 11u), values, position);
  }
}

template <unsigned Count>
void vector_depth_span_oracle(loom::type::buffer<unsigned> previous,
                              loom::type::buffer<unsigned> output,
                              unsigned count, unsigned depth, unsigned step) {
  static_cast<void>(count);
  if constexpr (Count > 0u) {
    auto previous_values = loom::buffer::view<Count>(
        previous, {}, loom::encoding::layout::dense<1>());
    auto output_values = loom::buffer::view<Count>(
        output, {}, loom::encoding::layout::dense<1>());
    for (unsigned position = 0; position < Count; ++position) {
      unsigned previous_value = loom::view::load(previous_values, position);
      loom::view::store(
          vector_depth_expected(previous_value, depth + position * step),
          output_values, position);
    }
  } else {
    static_cast<void>(previous);
    static_cast<void>(output);
    static_cast<void>(depth);
    static_cast<void>(step);
  }
}

LOOM_CHECK_SCENARIO(vector_depth_values) {
  loom::check::trial<1>(
      [](loom::check::ordinal trial, loom::check::entropy entropy) {
        const auto previous = loom::check::fill<unsigned, 16>(0u);
        const auto depth = loom::check::fill<unsigned, 16>(0u);
        loom::check::generate<vector_depth_inputs<16>>(previous, depth);
        const auto storage = loom::check::fill<unsigned, 48>(-123u);
        const auto output = loom::check::slice<16>(storage, 16);
        loom::check::compare<vector_depth, vector_depth_oracle<16>>(
            previous, depth, output, 1u, [&] {
              loom::check::expect_bitwise(previous, previous);
              loom::check::expect_bitwise(depth, depth);
              loom::check::expect_bitwise(storage, storage);
            });
      });
  loom::check::trial<1>(
      [](loom::check::ordinal trial, loom::check::entropy entropy) {
        const auto previous = loom::check::fill<unsigned, 48>(0u);
        const auto depth = loom::check::fill<unsigned, 48>(0u);
        loom::check::generate<vector_depth_inputs<48>>(previous, depth);
        const auto storage = loom::check::fill<unsigned, 80>(-123u);
        const auto output = loom::check::slice<48>(storage, 16);
        loom::check::compare<vector_depth, vector_depth_oracle<48>>(
            previous, depth, output, 3u, [&] {
              loom::check::expect_bitwise(previous, previous);
              loom::check::expect_bitwise(depth, depth);
              loom::check::expect_bitwise(storage, storage);
            });
      });
  loom::check::trial<1>(
      [](loom::check::ordinal trial, loom::check::entropy entropy) {
        const auto previous = loom::check::fill<unsigned, 112>(0u);
        const auto depth = loom::check::fill<unsigned, 112>(0u);
        loom::check::generate<vector_depth_inputs<112>>(previous, depth);
        const auto storage = loom::check::fill<unsigned, 144>(-123u);
        const auto output = loom::check::slice<112>(storage, 16);
        loom::check::compare<vector_depth, vector_depth_oracle<112>>(
            previous, depth, output, 7u, [&] {
              loom::check::expect_bitwise(previous, previous);
              loom::check::expect_bitwise(depth, depth);
              loom::check::expect_bitwise(storage, storage);
            });
      });
}

LOOM_CHECK_SCENARIO(vector_depth_span_values) {
  loom::check::trial<1>(
      [](loom::check::ordinal trial, loom::check::entropy entropy) {
        const auto previous = loom::check::fill<unsigned, 1>(0u);
        loom::check::generate<vector_depth_span_input<1>>(previous);
        const auto storage = loom::check::fill<unsigned, 32>(-123u);
        const auto output = loom::check::slice<0>(storage, 16);
        loom::check::compare<vector_depth_span, vector_depth_span_oracle<0>>(
            previous, output, 0u, 0xffff1234u, 0x137cfu, [&] {
              loom::check::expect_bitwise(previous, previous);
              loom::check::expect_bitwise(storage, storage);
            });
      });
  loom::check::trial<1>(
      [](loom::check::ordinal trial, loom::check::entropy entropy) {
        const auto previous = loom::check::fill<unsigned, 1>(0u);
        loom::check::generate<vector_depth_span_input<1>>(previous);
        const auto storage = loom::check::fill<unsigned, 33>(-123u);
        const auto output = loom::check::slice<1>(storage, 16);
        loom::check::compare<vector_depth_span, vector_depth_span_oracle<1>>(
            previous, output, 1u, 0xffff1234u, 0x137cfu, [&] {
              loom::check::expect_bitwise(previous, previous);
              loom::check::expect_bitwise(storage, storage);
            });
      });
  loom::check::trial<1>(
      [](loom::check::ordinal trial, loom::check::entropy entropy) {
        const auto previous = loom::check::fill<unsigned, 15>(0u);
        loom::check::generate<vector_depth_span_input<15>>(previous);
        const auto storage = loom::check::fill<unsigned, 47>(-123u);
        const auto output = loom::check::slice<15>(storage, 16);
        loom::check::compare<vector_depth_span, vector_depth_span_oracle<15>>(
            previous, output, 15u, 0xffff1234u, 0x137cfu, [&] {
              loom::check::expect_bitwise(previous, previous);
              loom::check::expect_bitwise(storage, storage);
            });
      });
  loom::check::trial<1>(
      [](loom::check::ordinal trial, loom::check::entropy entropy) {
        const auto previous = loom::check::fill<unsigned, 16>(0u);
        loom::check::generate<vector_depth_span_input<16>>(previous);
        const auto storage = loom::check::fill<unsigned, 48>(-123u);
        const auto output = loom::check::slice<16>(storage, 16);
        loom::check::compare<vector_depth_span, vector_depth_span_oracle<16>>(
            previous, output, 16u, 0xffff1234u, 0x137cfu, [&] {
              loom::check::expect_bitwise(previous, previous);
              loom::check::expect_bitwise(storage, storage);
            });
      });
  loom::check::trial<1>(
      [](loom::check::ordinal trial, loom::check::entropy entropy) {
        const auto previous = loom::check::fill<unsigned, 17>(0u);
        loom::check::generate<vector_depth_span_input<17>>(previous);
        const auto storage = loom::check::fill<unsigned, 49>(-123u);
        const auto output = loom::check::slice<17>(storage, 16);
        loom::check::compare<vector_depth_span, vector_depth_span_oracle<17>>(
            previous, output, 17u, 0xffff1234u, 0x137cfu, [&] {
              loom::check::expect_bitwise(previous, previous);
              loom::check::expect_bitwise(storage, storage);
            });
      });
  loom::check::trial<1>(
      [](loom::check::ordinal trial, loom::check::entropy entropy) {
        const auto previous = loom::check::fill<unsigned, 31>(0u);
        loom::check::generate<vector_depth_span_input<31>>(previous);
        const auto storage = loom::check::fill<unsigned, 63>(-123u);
        const auto output = loom::check::slice<31>(storage, 16);
        loom::check::compare<vector_depth_span, vector_depth_span_oracle<31>>(
            previous, output, 31u, 0xffff1234u, 0x137cfu, [&] {
              loom::check::expect_bitwise(previous, previous);
              loom::check::expect_bitwise(storage, storage);
            });
      });
  loom::check::trial<1>(
      [](loom::check::ordinal trial, loom::check::entropy entropy) {
        const auto previous = loom::check::fill<unsigned, 32>(0u);
        loom::check::generate<vector_depth_span_input<32>>(previous);
        const auto storage = loom::check::fill<unsigned, 64>(-123u);
        const auto output = loom::check::slice<32>(storage, 16);
        loom::check::compare<vector_depth_span, vector_depth_span_oracle<32>>(
            previous, output, 32u, 0xffff1234u, 0x137cfu, [&] {
              loom::check::expect_bitwise(previous, previous);
              loom::check::expect_bitwise(storage, storage);
            });
      });
  loom::check::trial<1>(
      [](loom::check::ordinal trial, loom::check::entropy entropy) {
        const auto previous = loom::check::fill<unsigned, 129>(0u);
        loom::check::generate<vector_depth_span_input<129>>(previous);
        const auto storage = loom::check::fill<unsigned, 161>(-123u);
        const auto output = loom::check::slice<129>(storage, 16);
        loom::check::compare<vector_depth_span, vector_depth_span_oracle<129>>(
            previous, output, 129u, 0xffff1234u, 0x137cfu, [&] {
              loom::check::expect_bitwise(previous, previous);
              loom::check::expect_bitwise(storage, storage);
            });
      });
}
