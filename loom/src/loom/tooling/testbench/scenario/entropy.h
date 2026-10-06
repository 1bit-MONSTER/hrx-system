// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Immutable deterministic entropy identities for check.scenario execution.

#ifndef LOOM_TOOLING_TESTBENCH_SCENARIO_ENTROPY_H_
#define LOOM_TOOLING_TESTBENCH_SCENARIO_ENTROPY_H_

#include "iree/base/api.h"

#ifdef __cplusplus
extern "C" {
#endif

// A position-independent entropy identity.
//
// Identities never carry a mutable cursor. Named forks and ordinal derivations
// are pure functions, so campaign sharding and traversal order cannot change a
// trial's generated values.
typedef struct loom_testbench_entropy_t {
  // First half of the 128-bit entropy identity.
  uint64_t low;
  // Second half of the 128-bit entropy identity.
  uint64_t high;
} loom_testbench_entropy_t;

// Derives the root identity for a campaign seed.
loom_testbench_entropy_t loom_testbench_entropy_root(uint64_t seed);

// Derives a stable named child identity without changing |parent|.
loom_testbench_entropy_t loom_testbench_entropy_fork(
    loom_testbench_entropy_t parent, iree_string_view_t name);

// Derives a stable ordinal child identity without changing |parent|.
loom_testbench_entropy_t loom_testbench_entropy_at(
    loom_testbench_entropy_t parent, uint64_t ordinal);

// Reads one deterministic word at |ordinal| without changing |entropy|.
uint64_t loom_testbench_entropy_read(loom_testbench_entropy_t entropy,
                                     uint64_t ordinal);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TOOLING_TESTBENCH_SCENARIO_ENTROPY_H_
