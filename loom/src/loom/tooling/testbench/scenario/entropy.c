// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/tooling/testbench/scenario/entropy.h"

#include "iree/base/internal/math.h"

// SplitMix64's stateless finalizer. All arithmetic is explicitly unsigned so
// the entropy mapping is stable across hosts and compiler implementations.
static uint64_t loom_testbench_entropy_mix(uint64_t value) {
  value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
  value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
  return value ^ (value >> 31);
}

static uint64_t loom_testbench_entropy_hash_name(uint64_t state,
                                                 iree_string_view_t name) {
  // Include the length and a domain constant before the bytes so forks remain
  // distinct from ordinal derivations even when their numeric payloads match.
  state ^=
      loom_testbench_entropy_mix((uint64_t)name.size ^ 0x4E414D45444C4F4Full);
  for (iree_host_size_t i = 0; i < name.size; ++i) {
    state ^= (uint8_t)name.data[i];
    state *= 0x100000001B3ull;
  }
  return loom_testbench_entropy_mix(state);
}

loom_testbench_entropy_t loom_testbench_entropy_root(uint64_t seed) {
  return (loom_testbench_entropy_t){
      .low = loom_testbench_entropy_mix(seed ^ 0x524F4F544C4F4F4Dull),
      .high = loom_testbench_entropy_mix(seed ^ 0x6D6F6F6C544F4F52ull),
  };
}

loom_testbench_entropy_t loom_testbench_entropy_fork(
    loom_testbench_entropy_t parent, iree_string_view_t name) {
  return (loom_testbench_entropy_t){
      .low = loom_testbench_entropy_hash_name(
          parent.low ^ iree_math_rotl_u64(parent.high, 17) ^
              0x464F524B4C4F4F4Dull,
          name),
      .high = loom_testbench_entropy_hash_name(
          parent.high ^ iree_math_rotl_u64(parent.low, 41) ^
              0x6D6F6F6C4B524F46ull,
          name),
  };
}

loom_testbench_entropy_t loom_testbench_entropy_at(
    loom_testbench_entropy_t parent, uint64_t ordinal) {
  const uint64_t low_ordinal =
      loom_testbench_entropy_mix(ordinal ^ 0x4F5244494E414C30ull);
  const uint64_t high_ordinal =
      loom_testbench_entropy_mix(ordinal ^ 0x314C414E4944524Full);
  return (loom_testbench_entropy_t){
      .low = loom_testbench_entropy_mix(
          parent.low ^ iree_math_rotl_u64(parent.high, 17) ^ low_ordinal),
      .high = loom_testbench_entropy_mix(
          parent.high ^ iree_math_rotl_u64(parent.low, 41) ^ high_ordinal),
  };
}

uint64_t loom_testbench_entropy_read(loom_testbench_entropy_t entropy,
                                     uint64_t ordinal) {
  const loom_testbench_entropy_t position =
      loom_testbench_entropy_at(entropy, ordinal);
  return loom_testbench_entropy_mix(position.low ^
                                    iree_math_rotl_u64(position.high, 23) ^
                                    0x524541444C4F4F4Dull);
}
