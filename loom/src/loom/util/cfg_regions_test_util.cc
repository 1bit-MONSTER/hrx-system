// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/util/cfg_regions_test_util.h"

#include <algorithm>
#include <set>

namespace loom::testing {

CfgRegionsTestHarness::CfgRegionsTestHarness() {
  iree_arena_block_pool_initialize(4096, iree_allocator_system(), &pool_);
  iree_arena_initialize(&pool_, &arena_);
}

CfgRegionsTestHarness::~CfgRegionsTestHarness() {
  iree_arena_deinitialize(&arena_);
  iree_arena_block_pool_deinitialize(&pool_);
}

iree_status_t CfgRegionsTestHarness::Build(const CfgGraph& fixture,
                                           loom_cfg_regions_t* out_regions) {
  iree_arena_reset(&arena_);
  IREE_RETURN_IF_ERROR(
      loom_cfg_dominance_build(fixture.get(), &arena_, &dominance_));
  return loom_cfg_regions_build(fixture.get(), &dominance_, &arena_,
                                out_regions);
}

iree_status_t CfgRegionsTestHarness::Check(
    const std::vector<std::vector<uint16_t>>& successors) {
  CfgGraph fixture(successors);
  loom_cfg_regions_t regions;
  IREE_RETURN_IF_ERROR(Build(fixture, &regions));
  auto* graph = fixture.get();
  auto reachable = [&](uint16_t removed) {
    std::vector<bool> reached(successors.size(), false);
    std::vector<uint16_t> pending;
    if (!successors.empty() && removed != 0) {
      pending.push_back(0);
    }
    while (!pending.empty()) {
      uint16_t block = pending.back();
      pending.pop_back();
      if (block == removed || reached[block]) {
        continue;
      }
      reached[block] = true;
      pending.insert(pending.end(), successors[block].begin(),
                     successors[block].end());
    }
    return reached;
  };
  auto live = reachable(UINT16_MAX);
  for (uint16_t entry = 0; entry < successors.size(); ++entry) {
    auto without_entry = reachable(entry);
    std::vector<bool> members(successors.size(), false);
    std::set<uint32_t> destinations;
    size_t return_count = 0;
    for (size_t block = 0; block < successors.size(); ++block) {
      members[block] = live[block] && !without_entry[block];
      if (members[block] && successors[block].empty()) {
        destinations.insert(UINT16_MAX);
        ++return_count;
      }
    }
    std::vector<loom_cfg_edge_index_t> exits;
    for (uint32_t edge = 0; edge < graph->edge_count; ++edge) {
      const auto& info = graph->edges[edge];
      if (members[info.source_block_index] &&
          !members[info.target_block_index]) {
        destinations.insert(info.target_block_index);
        exits.push_back(edge);
      }
    }
    const auto& region = regions.blocks[entry];
    if (region.exit_count != exits.size() + return_count) {
      return iree_make_status(IREE_STATUS_INTERNAL,
                              "exit count mismatch at entry %u", entry);
    }
    for (uint16_t destination = 0; destination < successors.size();
         ++destination) {
      std::vector<uint32_t> expected;
      for (uint32_t edge = 0; edge < graph->edge_count; ++edge) {
        const auto& info = graph->edges[edge];
        if (members[info.source_block_index] &&
            info.target_block_index == destination) {
          expected.push_back(edge);
        }
      }
      auto span = loom_cfg_regions_edges_to(graph, &dominance_, &regions, entry,
                                            destination);
      std::vector<uint32_t> actual;
      if (span.count) {
        actual.assign(span.values, span.values + span.count);
      }
      std::sort(actual.begin(), actual.end());
      if (actual != expected) {
        return iree_make_status(
            IREE_STATUS_INTERNAL,
            "destination edge mismatch at entry %u, destination %u", entry,
            destination);
      }
    }
    if (destinations.size() != 1 || *destinations.begin() == UINT16_MAX) {
      if (region.continuation_index != LOOM_CFG_REGION_CONTINUATION_NONE ||
          region.exit_edges.count != 0) {
        return iree_make_status(IREE_STATUS_INTERNAL,
                                "unexpected continuation at entry %u", entry);
      }
      continue;
    }
    if (region.continuation_index != *destinations.begin()) {
      return iree_make_status(IREE_STATUS_INTERNAL,
                              "continuation mismatch at entry %u", entry);
    }
    std::vector<loom_cfg_edge_index_t> actual(
        region.exit_edges.values,
        region.exit_edges.values + region.exit_edges.count);
    std::sort(actual.begin(), actual.end());
    if (actual != exits) {
      return iree_make_status(IREE_STATUS_INTERNAL,
                              "exit edge mismatch at entry %u", entry);
    }
  }
  return iree_ok_status();
}

}  // namespace loom::testing
