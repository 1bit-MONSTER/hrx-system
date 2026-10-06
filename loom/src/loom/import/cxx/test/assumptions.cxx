// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <hip/hip_runtime.h>

[[loom::force_inline]] unsigned bound_pair(unsigned first, unsigned second) {
  __builtin_assume(first < 256u && second < 256u);
  return first * 257u + second;
}

[[loom::force_inline]] unsigned bound_seven(unsigned a, unsigned b, unsigned c,
                                            unsigned d, unsigned e, unsigned f,
                                            unsigned g) {
  loom::assume(a < 256u && b < 256u && c < 256u && d < 256u && e < 256u &&
               f < 256u && g < 256u);
  return a + 2u * b + 3u * c + 5u * d + 7u * e + 11u * f + 13u * g;
}

template <unsigned Limit>
[[loom::force_inline]] unsigned refine(unsigned value) {
  loom::assume(((value < 256u) && ((value) < (1u << Limit))) && value < 64u);
  return value * 17u;
}

[[loom::force_inline]] unsigned bound_repeated(unsigned value) {
  return refine<5>(value);
}

[[loom::force_inline]] unsigned bound_capacity(unsigned value) {
  constexpr unsigned capacity = 28672;
  constexpr unsigned stride = 16;
  loom::assume(value <
               ((capacity / sizeof(unsigned) - 16u - 320u) / stride + 1u));
  return value * 16u + 336u;
}

[[loom::force_inline]] unsigned bound_cast(unsigned value) {
  loom::assume(value < static_cast<unsigned char>(272u));
  return value + 5u;
}

[[loom::force_inline]] unsigned bound_byte(unsigned char value) {
  loom::assume(value < 256u);
  return value + (value >= 128u ? 1024u : 0u);
}

[[loom::force_inline]] unsigned bound_wide(unsigned long long value) {
  loom::assume(value < (0xffffffffu + 257u));
  return (unsigned)(value * 3u);
}

[[loom::force_inline]] unsigned bound_size(unsigned value) {
  loom::assume(value < sizeof(++value) * 4u);
  return value;
}

[[loom::force_inline]] unsigned bound_scoped(unsigned value) {
  if (value < 256u) {
    loom::assume(value < 256u);
    return value + 1u;
  }
  return value + 3u;
}

[[loom::force_inline]] unsigned bound_inclusive(unsigned tokens) {
  constexpr unsigned capacity = 427u;
  loom::assume(tokens <= capacity);
  return tokens * 19u + 3u;
}

[[loom::force_inline]] int bound_signed(int hidden, int capacity) {
  loom::assume(hidden > 0 && hidden <= capacity);
  return hidden * 23 + capacity;
}

[[loom::force_inline]] unsigned bound_unsigned(unsigned tokens,
                                               unsigned capacity) {
  loom::assume(tokens <= capacity);
  return tokens ^ capacity;
}

[[loom::kernel, loom::workgroup_size(64, 1, 1), loom::workgroup_count(1, 1, 1)]]
void assumption_kernel(unsigned* output, unsigned input) {
  unsigned lane = threadIdx.x;
  unsigned value = input + lane;
  unsigned output_offset = lane * 12u;
  output[output_offset] = bound_pair(value & 255u, (value >> 8u) & 255u);
  output[output_offset + 1u] =
      bound_seven(value & 255u, 1u, 2u, 3u, 5u, 7u, 11u);
  output[output_offset + 2u] = bound_repeated(value & 31u);
  output[output_offset + 3u] = bound_capacity(value % 428u);
  output[output_offset + 4u] = bound_cast(value & 15u);
  output[output_offset + 5u] = bound_byte((unsigned char)value);
  output[output_offset + 6u] = bound_wide(value & 255u);
  output[output_offset + 7u] = bound_size(value & 15u);
  output[output_offset + 8u] = bound_scoped(value);
  output[output_offset + 9u] = bound_inclusive(value % 428u);
  int hidden = (int)(value % 63u) + 1;
  int capacity = hidden + (int)(value % 5u);
  output[output_offset + 10u] = (unsigned)bound_signed(hidden, capacity);
  unsigned tokens = value & 0x7fffffffu;
  unsigned token_capacity = tokens | 0x80000000u;
  output[output_offset + 11u] = bound_unsigned(tokens, token_capacity);
}
