// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <loomcxx/check.h>
#include <loomcxx/kernel.h>

using Bytes4 = unsigned char __attribute__((ext_vector_type(4)));
using Codes4 = signed char __attribute__((ext_vector_type(4)));
using Codebook16 = signed char __attribute__((ext_vector_type(16)));

static Codes4 lookup(Codebook16 table, Bytes4 indices) {
  return {table[indices[0]], table[indices[1]], table[indices[2]],
          table[indices[3]]};
}

[[loom::kernel, loom::workgroup_size(1, 1, 1), loom::workgroup_count(1, 1, 1)]]
void decode_codes(const Bytes4* input, Codes4* output) {
  constexpr Codebook16 table = {-127, -104, -83, -65, -49, -35, -22, -10,
                                1,    13,   25,  38,  53,  69,  89,  113};
  Bytes4 packed = input[0];
  output[0] = lookup(table, packed & 15);
  output[1] = lookup(table, packed >> 4);
}

LOOM_CHECK_CASE(packed_code_indices) {
  // Each packed result selects across codebook quarters. The high-nibble
  // lookup requires the zero-fill bound to survive C++ integer promotion.
  const auto input = loom::check::fill<unsigned, 1>(0xFECB9854u);
  const auto storage = loom::check::fill<unsigned, 4>(0xA5A5A5A5u);
  const auto output = loom::check::slice<2>(storage, 1);
  loom::check::launch<decode_codes>(input, output);
  // Indices [4, 8, 11, 14] select [-49, 1, 38, 89].
  loom::check::expect_bitwise(loom::check::slice<1>(output, 0),
                              loom::check::fill<unsigned, 1>(0x592601CFu));
  // Indices [5, 9, 12, 15] select [-35, 13, 53, 113].
  loom::check::expect_bitwise(loom::check::slice<1>(output, 1),
                              loom::check::fill<unsigned, 1>(0x71350DDDu));
  loom::check::expect_bitwise(loom::check::slice<1>(storage, 0),
                              loom::check::fill<unsigned, 1>(0xA5A5A5A5u));
  loom::check::expect_bitwise(loom::check::slice<1>(storage, 3),
                              loom::check::fill<unsigned, 1>(0xA5A5A5A5u));
}
