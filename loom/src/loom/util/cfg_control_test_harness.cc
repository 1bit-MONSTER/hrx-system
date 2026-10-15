// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/util/cfg_control_test_harness.h"

#include "loom/util/cfg_control_test_util.h"
#include "loom/util/cfg_graph_test_util.h"

namespace loom::testing {

CfgControlTestHarness::CfgControlTestHarness() {
  iree_arena_block_pool_initialize(4096, iree_allocator_system(), &pool_);
  iree_arena_initialize(&pool_, &arena_);
}

CfgControlTestHarness::~CfgControlTestHarness() {
  iree_arena_deinitialize(&arena_);
  iree_arena_block_pool_deinitialize(&pool_);
}

iree_status_t CfgControlTestHarness::CheckStructure(
    const std::vector<std::vector<uint16_t>>& successors) {
  CfgGraph fixture(successors);
  loom_cfg_control_t structure;
  iree_status_t status =
      loom_cfg_control_build(fixture.get(), &arena_, &structure);
  if (iree_status_is_ok(status)) {
    CfgControlOracle oracle(fixture.get());
    status = oracle.CheckStructure(structure);
  }
  iree_arena_reset(&arena_);
  return status;
}

iree_status_t CfgControlTestHarness::CheckPostdominance(
    const std::vector<std::vector<uint16_t>>& successors) {
  CfgGraph fixture(successors);
  const auto* graph = fixture.get();
  const size_t exit = graph->block_count;
  const size_t count = exit + 1;
  loom_cfg_postdominance_t postdominance;
  iree_status_t status =
      loom_cfg_postdominance_build(graph, &arena_, &postdominance);
  if (iree_status_is_ok(status) && !postdominance.available) {
    status = iree_make_status(IREE_STATUS_INTERNAL,
                              "valid postdominance unavailable");
  }
  if (iree_status_is_ok(status) && postdominance.exit_node != exit) {
    status = iree_make_status(IREE_STATUS_INTERNAL,
                              "exit node mismatch: expected %zu, got %u", exit,
                              postdominance.exit_node);
  }
  if (iree_status_is_ok(status)) {
    CfgControlOracle oracle(graph);
    for (size_t source = 0; source < count; ++source) {
      const auto& expected = oracle.postdominators()[source];
      const auto& actual = postdominance.nodes[source];
      if (actual.immediate_postdominator != expected.immediate_postdominator ||
          actual.depth != expected.depth) {
        status =
            iree_make_status(IREE_STATUS_INTERNAL,
                             "postdominator mismatch at source %zu", source);
        break;
      }
    }
  }
  iree_arena_reset(&arena_);
  return status;
}

iree_status_t CfgControlTestHarness::CheckFactUpdates(
    const std::vector<std::vector<uint16_t>>& successors,
    std::mt19937& random) {
  CfgGraph fixture(successors);
  loom_cfg_control_t structure;
  iree_status_t status =
      loom_cfg_control_build(fixture.get(), &arena_, &structure);
  loom_value_fact_control_t control;
  if (iree_status_is_ok(status)) {
    status = loom_value_fact_control_initialize(&structure, &arena_, &control);
  }
  const size_t allocation_size = arena_.used_allocation_size;
  std::vector<uint8_t> selectors(successors.size(), 4);
  CfgControlOracle oracle(fixture.get());
  if (iree_status_is_ok(status)) {
    status = oracle.CheckStructure(structure);
  }
  auto previous = oracle.Solve(selectors);
  for (uint32_t batch = 0; batch < 30 && iree_status_is_ok(status); ++batch) {
    const uint32_t count = 1 + random() % successors.size();
    for (uint32_t i = 0; i < count; ++i) {
      const uint32_t block = random() % successors.size();
      selectors[block] = random() % 5;
      loom_value_fact_control_set_selector(
          &control, block, ControlDistribution(selectors[block]));
    }
    loom_value_fact_control_settle(&control);
    status = oracle.CheckFacts(&control, selectors);
    if (iree_status_is_ok(status)) {
      status = oracle.CheckPublication(&control, selectors, &previous);
    }
    if (iree_status_is_ok(status) &&
        arena_.used_allocation_size != allocation_size) {
      status = iree_make_status(IREE_STATUS_INTERNAL,
                                "control update allocated memory");
    }
    if (!iree_status_is_ok(status)) {
      status = iree_status_annotate_f(status, "batch %u", batch);
    }
  }
  iree_arena_reset(&arena_);
  return status;
}

}  // namespace loom::testing
