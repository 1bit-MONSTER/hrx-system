// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <hip/hip_runtime.h>
#include <loomcxx/check.h>
#include <loomcxx/view.h>

// Uniform and lane-varying locals exercise both native register placements.
__global__
    [[loom::workgroup_size(64, 1, 1), loom::workgroup_count(1, 1, 1)]] void
    increment_u8(unsigned char* output, unsigned input) {
  unsigned lane = threadIdx.x;
  unsigned char uniform = (unsigned char)input;
  ++uniform;
  output[lane * 2u] = uniform;
  unsigned char varying = (unsigned char)(input + lane);
  varying++;
  output[lane * 2u + 1u] = varying;
}

__global__
    [[loom::workgroup_size(64, 1, 1), loom::workgroup_count(1, 1, 1)]] void
    increment_u64(unsigned long long* output, unsigned long long input) {
  unsigned lane = threadIdx.x;
  unsigned long long uniform = input;
  ++uniform;
  output[lane * 2u] = uniform;
  unsigned long long varying = input + lane;
  varying++;
  output[lane * 2u + 1u] = varying;
}

unsigned increment_u8_input(loom::check::ordinal trial) {
  if (trial == 0) {
    return 0u;
  }
  if (trial == 1) {
    return 127u;
  }
  if (trial == 2) {
    return 128u;
  }
  if (trial == 3) {
    return 254u;
  }
  if (trial == 4) {
    return 255u;
  }
  return 511u;
}

unsigned long long increment_u64_input(loom::check::ordinal trial) {
  if (trial == 0) {
    return 0ull;
  }
  if (trial == 1) {
    return 126ull;
  }
  if (trial == 2) {
    return 254ull;
  }
  if (trial == 3) {
    return 255ull;
  }
  if (trial == 4) {
    return 0xffffffffull;
  }
  if (trial == 5) {
    return 0x100000000ull;
  }
  if (trial == 6) {
    return 0x7fffffffffffffffull;
  }
  if (trial == 7) {
    return 0x8000000000000000ull;
  }
  return 0xffffffffffffffffull;
}

void increment_u8_oracle(loom::type::buffer<unsigned char> output,
                         unsigned input) {
  auto output_view =
      loom::buffer::view<128>(output, {}, loom::encoding::layout::dense<1>());
  unsigned char uniform = (unsigned char)input;
  ++uniform;
  for (unsigned lane = 0; lane < 64u; ++lane) {
    unsigned char varying = (unsigned char)(input + lane);
    ++varying;
    loom::view::store(uniform, output_view, lane * 2u);
    loom::view::store(varying, output_view, lane * 2u + 1u);
  }
}

void increment_u64_oracle(loom::type::buffer<unsigned long long> output,
                          unsigned long long input) {
  auto output_view =
      loom::buffer::view<128>(output, {}, loom::encoding::layout::dense<1>());
  unsigned long long uniform = input;
  ++uniform;
  for (unsigned lane = 0; lane < 64u; ++lane) {
    unsigned long long varying = input + lane;
    ++varying;
    loom::view::store(uniform, output_view, lane * 2u);
    loom::view::store(varying, output_view, lane * 2u + 1u);
  }
}

LOOM_CHECK_SCENARIO(increment_values) {
  loom::check::trial<6>([](loom::check::ordinal trial,
                           loom::check::entropy entropy) {
    const auto input = loom::check::generate<increment_u8_input>(trial);
    const auto storage =
        loom::check::fill<160, unsigned char>((unsigned char)-123);
    const auto output = loom::check::slice<128>(storage, 16);
    loom::check::compare<increment_u8, increment_u8_oracle>(
        output, input, [&] { loom::check::expect_bitwise(storage, storage); });
  });
  loom::check::trial<9>([](loom::check::ordinal trial,
                           loom::check::entropy entropy) {
    const auto input = loom::check::generate<increment_u64_input>(trial);
    const auto storage =
        loom::check::fill<160, unsigned long long>((unsigned long long)-123ll);
    const auto output = loom::check::slice<128>(storage, 16);
    loom::check::compare<increment_u64, increment_u64_oracle>(
        output, input, [&] { loom::check::expect_bitwise(storage, storage); });
  });
}
