// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/util/cfg_loop_nest_test_util.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace loom::testing {

iree_status_t CheckLoopNest(const loom_cfg_loop_nest_t& nest) {
  const auto* graph = nest.graph;
  const size_t count = graph->block_count;
  std::vector<std::vector<bool>> membership;
  std::vector<uint16_t> headers;
  std::vector<bool> natural_backedges(graph->edge_count);
  for (uint16_t block = 0; block < count; ++block) {
    uint16_t loop_index = loom_cfg_loop_nest_innermost(&nest, block);
    if (loop_index != LOOM_CFG_LOOP_NEST_NONE &&
        loop_index >= nest.loop_count) {
      return iree_make_status(IREE_STATUS_INTERNAL, "invalid block membership");
    }
  }
  for (uint16_t header = 0; header < count; ++header) {
    if (!graph->blocks[header].reachable) {
      continue;
    }
    std::vector<bool> reached(count);
    std::vector<uint16_t> pending{0};
    while (!pending.empty()) {
      uint16_t block = pending.back();
      pending.pop_back();
      if (block == header || reached[block]) {
        continue;
      }
      reached[block] = true;
      auto outgoing = loom_cfg_graph_successors(graph, block);
      for (size_t i = 0; i < outgoing.count; ++i) {
        pending.push_back(outgoing.values[i]);
      }
    }
    std::vector<uint32_t> entries;
    std::vector<uint32_t> backedges;
    auto predecessors = loom_cfg_graph_predecessor_edges(graph, header);
    for (size_t i = 0; i < predecessors.count; ++i) {
      uint32_t edge = predecessors.values[i];
      uint16_t source = graph->edges[edge].source_block_index;
      if (!graph->blocks[source].reachable) {
        continue;
      }
      if (!reached[source]) {
        backedges.push_back(edge);
        natural_backedges[edge] = true;
      } else {
        entries.push_back(edge);
      }
    }
    if (backedges.empty()) {
      continue;
    }
    uint16_t loop_index = loom_cfg_loop_nest_innermost(&nest, header);
    if (loop_index == LOOM_CFG_LOOP_NEST_NONE ||
        nest.loops[loop_index].header_index != header) {
      return iree_make_status(IREE_STATUS_INTERNAL, "missing loop at header %u",
                              header);
    }
    const auto& loop = nest.loops[loop_index];
    std::vector<bool> members(count);
    members[header] = true;
    for (uint32_t edge : backedges) {
      pending.push_back(graph->edges[edge].source_block_index);
    }
    while (!pending.empty()) {
      uint16_t block = pending.back();
      pending.pop_back();
      if (members[block] || !graph->blocks[block].reachable) {
        continue;
      }
      members[block] = true;
      auto incoming = loom_cfg_graph_predecessors(graph, block);
      for (size_t i = 0; i < incoming.count; ++i) {
        pending.push_back(incoming.values[i]);
      }
    }
    std::vector<uint32_t> exits;
    uint32_t direct_exit_count = 0;
    for (size_t i = 0; i < graph->edge_count; ++i) {
      const auto& edge = graph->edges[i];
      if (members[edge.source_block_index] &&
          !members[edge.target_block_index]) {
        exits.push_back(i);
        if (loom_cfg_loop_nest_innermost(&nest, edge.source_block_index) ==
            loop_index) {
          ++direct_exit_count;
        }
      }
    }
    auto edges_match = [](const loom_cfg_loop_edge_summary_t& summary,
                          const std::vector<uint32_t>& edges) {
      return summary.count == edges.size() &&
             summary.unique_index ==
                 (edges.size() == 1 ? edges[0] : LOOM_CFG_EDGE_INDEX_INVALID);
    };
    if (!edges_match(loop.entries, entries) ||
        !edges_match(loop.backedges, backedges) ||
        !edges_match(loop.exits, exits)) {
      return iree_make_status(IREE_STATUS_INTERNAL,
                              "incorrect edges for header %u", header);
    }
    uint16_t continuation = LOOM_CFG_LOOP_CONTINUATION_NONE;
    if (!exits.empty()) {
      continuation = graph->edges[exits[0]].target_block_index;
      for (uint32_t edge : exits) {
        if (graph->edges[edge].target_block_index != continuation) {
          continuation = LOOM_CFG_LOOP_CONTINUATION_NONE;
          break;
        }
      }
    }
    if (loop.direct_exit_count != direct_exit_count ||
        loop.continuation_index != continuation) {
      return iree_make_status(IREE_STATUS_INTERNAL,
                              "incorrect exit ownership for header %u", header);
    }
    for (size_t block = 0; block < count; ++block) {
      if (loom_cfg_loop_nest_contains(&nest, loop_index, block) !=
          members[block]) {
        return iree_make_status(IREE_STATUS_INTERNAL,
                                "incorrect member %zu of header %u", block,
                                header);
      }
    }
    headers.push_back(header);
    membership.push_back(std::move(members));
  }
  if (nest.loop_count != membership.size()) {
    return iree_make_status(IREE_STATUS_INTERNAL, "incorrect loop count");
  }

  // Removing all natural backedges leaves a DAG exactly when the reachable
  // graph is reducible. This does not use the producer's DFS classification.
  std::vector<size_t> indegree(count);
  for (size_t i = 0; i < graph->edge_count; ++i) {
    const auto& edge = graph->edges[i];
    if (graph->blocks[edge.source_block_index].reachable &&
        !natural_backedges[i]) {
      ++indegree[edge.target_block_index];
    }
  }
  std::vector<uint16_t> ready;
  size_t reachable_count = 0;
  for (uint16_t block = 0; block < count; ++block) {
    if (!graph->blocks[block].reachable) {
      continue;
    }
    ++reachable_count;
    if (indegree[block] == 0) {
      ready.push_back(block);
    }
  }
  for (size_t i = 0; i < ready.size(); ++i) {
    auto outgoing = loom_cfg_graph_successor_edges(graph, ready[i]);
    for (size_t j = 0; j < outgoing.count; ++j) {
      uint32_t edge = outgoing.values[j];
      uint16_t target = graph->edges[edge].target_block_index;
      if (!natural_backedges[edge] && --indegree[target] == 0) {
        ready.push_back(target);
      }
    }
  }
  if (nest.reducible != (ready.size() == reachable_count)) {
    return iree_make_status(IREE_STATUS_INTERNAL, "incorrect reducibility");
  }

  // Immediate parents and innermost membership rule out skipped tree levels.
  for (size_t block = 0; block < count; ++block) {
    uint16_t expected = LOOM_CFG_LOOP_NEST_NONE;
    size_t smallest = count + 1;
    for (size_t i = 0; i < membership.size(); ++i) {
      size_t size =
          std::count(membership[i].begin(), membership[i].end(), true);
      if (membership[i][block] && size < smallest) {
        expected = loom_cfg_loop_nest_innermost(&nest, headers[i]);
        smallest = size;
      }
    }
    if (loom_cfg_loop_nest_innermost(&nest, block) != expected) {
      return iree_make_status(IREE_STATUS_INTERNAL,
                              "incorrect innermost loop for block %zu", block);
    }
  }
  for (size_t i = 0; i < membership.size(); ++i) {
    uint16_t expected = LOOM_CFG_LOOP_NEST_NONE;
    size_t smallest = count + 1;
    for (size_t j = 0; j < membership.size(); ++j) {
      if (i == j || !membership[j][headers[i]]) {
        continue;
      }
      size_t size =
          std::count(membership[j].begin(), membership[j].end(), true);
      if (size < smallest) {
        expected = loom_cfg_loop_nest_innermost(&nest, headers[j]);
        smallest = size;
      }
    }
    if (nest.loops[loom_cfg_loop_nest_innermost(&nest, headers[i])]
            .parent_loop_index != expected) {
      return iree_make_status(IREE_STATUS_INTERNAL,
                              "incorrect parent for header %u", headers[i]);
    }
  }
  return iree_ok_status();
}

}  // namespace loom::testing
