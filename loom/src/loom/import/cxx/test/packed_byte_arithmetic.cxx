// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <loomcxx/check.h>
#include <loomcxx/kernel.h>

using Bytes8 = unsigned char __attribute__((ext_vector_type(8)));

[[loom::kernel, loom::workgroup_size(1, 1, 1), loom::workgroup_count(1, 1, 1)]]
void double_bytes([[loom::noalias,
                    loom::assume_aligned(64)]] const Bytes8* input,
                  [[loom::noalias]] Bytes8* output) {
  *output = *input + *input;
}

[[loom::kernel, loom::workgroup_size(1, 1, 1), loom::workgroup_count(1, 1, 1)]]
void mixed_byte_arithmetic([[loom::noalias,
                             loom::assume_aligned(64)]] const Bytes8* input,
                           [[loom::noalias]] const Bytes8* other,
                           [[loom::noalias]] Bytes8* output) {
  // The two input pointers have independent alignment contracts.
  const Bytes8 left = *input;
  const Bytes8 right = *other;
  output[0] = left + right;
  output[1] = right + left;
  output[2] = left - right;
  output[3] = right - left;
  output[4] = left << 3;
  output[5] = left >> 5;
}

LOOM_CHECK_CASE(uniform_byte_arithmetic) {
  // Different bytes exercise both words, wrapping and cross-byte carries.
  const auto input =
      loom::check::fill<unsigned long long, 1>(0x55C04001FF807F00ull);
  const auto storage =
      loom::check::fill<unsigned long long, 3>(0xA5A5A5A5A5A5A5A5ull);
  const auto output = loom::check::slice<1>(storage, 1);
  loom::check::launch<double_bytes>(input, output);
  loom::check::expect_bitwise(
      output, loom::check::fill<unsigned long long, 1>(0xAA808002FE00FE00ull));
  loom::check::expect_bitwise(
      input, loom::check::fill<unsigned long long, 1>(0x55C04001FF807F00ull));
  loom::check::expect_bitwise(
      loom::check::slice<1>(storage, 0),
      loom::check::fill<unsigned long long, 1>(0xA5A5A5A5A5A5A5A5ull));
  loom::check::expect_bitwise(
      loom::check::slice<1>(storage, 2),
      loom::check::fill<unsigned long long, 1>(0xA5A5A5A5A5A5A5A5ull));
}

LOOM_CHECK_CASE(mixed_byte_arithmetic_banks) {
  const auto input =
      loom::check::fill<unsigned long long, 1>(0x55C04001FF807F00ull);
  const auto other =
      loom::check::fill<unsigned long long, 1>(0xC38112FE017F8001ull);
  const auto storage =
      loom::check::fill<unsigned long long, 8>(0xA5A5A5A5A5A5A5A5ull);
  const auto output = loom::check::slice<6>(storage, 1);
  loom::check::launch<mixed_byte_arithmetic>(input, other, output);
  loom::check::expect_bitwise(
      loom::check::slice<1>(output, 0),
      loom::check::fill<unsigned long long, 1>(0x184152FF00FFFF01ull));
  loom::check::expect_bitwise(
      loom::check::slice<1>(output, 1),
      loom::check::fill<unsigned long long, 1>(0x184152FF00FFFF01ull));
  loom::check::expect_bitwise(
      loom::check::slice<1>(output, 2),
      loom::check::fill<unsigned long long, 1>(0x923F2E03FE01FFFFull));
  loom::check::expect_bitwise(
      loom::check::slice<1>(output, 3),
      loom::check::fill<unsigned long long, 1>(0x6EC1D2FD02FF0101ull));
  loom::check::expect_bitwise(
      loom::check::slice<1>(output, 4),
      loom::check::fill<unsigned long long, 1>(0xA8000008F800F800ull));
  loom::check::expect_bitwise(
      loom::check::slice<1>(output, 5),
      loom::check::fill<unsigned long long, 1>(0x0206020007040300ull));
  loom::check::expect_bitwise(
      input, loom::check::fill<unsigned long long, 1>(0x55C04001FF807F00ull));
  loom::check::expect_bitwise(
      other, loom::check::fill<unsigned long long, 1>(0xC38112FE017F8001ull));
  loom::check::expect_bitwise(
      loom::check::slice<1>(storage, 0),
      loom::check::fill<unsigned long long, 1>(0xA5A5A5A5A5A5A5A5ull));
  loom::check::expect_bitwise(
      loom::check::slice<1>(storage, 7),
      loom::check::fill<unsigned long long, 1>(0xA5A5A5A5A5A5A5A5ull));
}
