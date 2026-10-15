// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_UTIL_CFG_CONTROL_TEST_HARNESS_H_
#define LOOM_UTIL_CFG_CONTROL_TEST_HARNESS_H_

#include <cstdint>
#include <random>
#include <vector>

#include "iree/base/internal/arena.h"

namespace loom::testing {

// Reusable production-analysis harness for deterministic and stress tests.
// Each check compares one CFG against CfgControlOracle and resets the shared
// arena before returning.
class CfgControlTestHarness {
 public:
  CfgControlTestHarness();
  ~CfgControlTestHarness();

  CfgControlTestHarness(const CfgControlTestHarness&) = delete;
  CfgControlTestHarness& operator=(const CfgControlTestHarness&) = delete;

  iree_status_t CheckStructure(
      const std::vector<std::vector<uint16_t>>& successors);
  iree_status_t CheckPostdominance(
      const std::vector<std::vector<uint16_t>>& successors);
  iree_status_t CheckFactUpdates(
      const std::vector<std::vector<uint16_t>>& successors,
      std::mt19937& random);

  // Analysis storage for focused tests that inspect production structures
  // directly instead of comparing a generated graph with the oracle.
  iree_arena_allocator_t* arena() { return &arena_; }

 private:
  // Reusable backing storage for analysis allocations.
  iree_arena_block_pool_t pool_;
  // One result lifetime, reset between generated graph cases.
  iree_arena_allocator_t arena_;
};

}  // namespace loom::testing

#endif  // LOOM_UTIL_CFG_CONTROL_TEST_HARNESS_H_
