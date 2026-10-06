// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <loomcxx/kernel.h>

typedef unsigned U16 __attribute__((vector_size(64)));

unsigned* choose_pointer(bool choose_first, unsigned* first, unsigned* second) {
  return choose_first ? first : second;
}

void select_buffers(unsigned* input, unsigned* output) {
  const unsigned count = input[0];
  const unsigned choice = input[1];
  loom::assume(count < 9u);
  for (unsigned word = 0; word < 256u; ++word) {
    output[word] = 0x6bad0000u + word;
  }

  // The two roots and their interior origins differ. Reads through either
  // original pointer must observe writes through the selected pointer.
  for (unsigned word = 0; word < 16u; ++word) {
    unsigned* first = input + 16u + word;
    unsigned* second = output + 64u + word;
    unsigned* selected =
        choose_pointer(((choice + word) & 1u) != 0u, first, second);
    *selected ^= 0x7f00ff80u;
    output[128u + word] = *selected;
    output[160u + word] = *first;
    output[192u + word] = *second;
  }

  // Zero iterations retain the first coordinate; successive iterations swap
  // both roots and origins, including a simultaneous backedge permutation.
  unsigned* current = input + 32u;
  unsigned* other = output + 96u;
  for (unsigned iteration = 0; iteration < count; ++iteration) {
    unsigned* previous = current;
    current = other;
    other = previous;
  }
  *reinterpret_cast<U16*>(output + 224u) =
      *reinterpret_cast<const U16*>(current);
  *reinterpret_cast<U16*>(output + 240u) = *reinterpret_cast<const U16*>(other);
}
