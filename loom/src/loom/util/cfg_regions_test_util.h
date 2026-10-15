// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_UTIL_CFG_REGIONS_TEST_UTIL_H_
#define LOOM_UTIL_CFG_REGIONS_TEST_UTIL_H_

#include <cstdint>
#include <vector>

#include "iree/base/internal/arena.h"
#include "loom/util/cfg_dominance.h"
#include "loom/util/cfg_graph_test_util.h"
#include "loom/util/cfg_regions.h"

namespace loom::testing {

// Reusable independent oracle for CFG region analysis tests. Vertex removal
// establishes domination without consulting the production tree or intervals;
// outgoing-edge enumeration then determines continuations and edge identities.
class CfgRegionsTestHarness {
 public:
  CfgRegionsTestHarness();
  ~CfgRegionsTestHarness();

  CfgRegionsTestHarness(const CfgRegionsTestHarness&) = delete;
  CfgRegionsTestHarness& operator=(const CfgRegionsTestHarness&) = delete;

  iree_status_t Build(const CfgGraph& fixture, loom_cfg_regions_t* out_regions);
  iree_status_t Check(const std::vector<std::vector<uint16_t>>& successors);

 private:
  // Allocation pool reused across independent CFG snapshots.
  iree_arena_block_pool_t pool_;
  // Storage retaining the current graph analysis result.
  iree_arena_allocator_t arena_;
  // Dominance snapshot sharing arena_ with its derived region index.
  loom_cfg_dominance_t dominance_;
};

}  // namespace loom::testing

#endif  // LOOM_UTIL_CFG_REGIONS_TEST_UTIL_H_
