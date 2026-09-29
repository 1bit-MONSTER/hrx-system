// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/codegen/low/schedule/source_suffix.h"

#include <string.h>

#include "iree/base/internal/math.h"

static bool loom_low_schedule_source_suffix_node_has_zero_issue_width(
    const loom_low_schedule_node_t* node) {
  return iree_any_bit_set(node->flags,
                          LOOM_LOW_SCHEDULE_NODE_FLAG_ZERO_ISSUE_WIDTH) ||
         loom_traits_are_compile_time_only(node->traits);
}

static uint32_t loom_low_schedule_source_suffix_edge_distance(
    const loom_low_schedule_dependency_index_t* dependency_index,
    uint32_t group_index) {
  const loom_low_schedule_dependency_group_t* group =
      loom_low_schedule_dependency_index_group_at(dependency_index,
                                                  group_index);
  return group->minimum_issue_separation_cycles > 0
             ? (uint32_t)group->minimum_issue_separation_cycles
             : 0;
}

static void loom_low_schedule_source_suffix_initialize_ranges(
    const loom_low_schedule_table_t* schedule, uint32_t* range_ends) {
  for (iree_host_size_t block_index = 0; block_index < schedule->block_count;
       ++block_index) {
    const loom_low_schedule_block_t* block = &schedule->blocks[block_index];
    const uint32_t block_end = block->node_start + block->node_count;
    uint32_t range_start = block->node_start;
    while (range_start < block_end) {
      uint32_t range_end = range_start + 1;
      if (!iree_any_bit_set(
              schedule->nodes[range_start].flags,
              LOOM_LOW_SCHEDULE_NODE_FLAG_SOURCE_ORDER_BOUNDARY)) {
        while (range_end < block_end &&
               !iree_any_bit_set(
                   schedule->nodes[range_end].flags,
                   LOOM_LOW_SCHEDULE_NODE_FLAG_SOURCE_ORDER_BOUNDARY)) {
          ++range_end;
        }
      }
      for (uint32_t node_index = range_start; node_index < range_end;
           ++node_index) {
        range_ends[node_index] = range_end;
      }
      range_start = range_end;
    }
  }
}

static void loom_low_schedule_source_suffix_compute_paths(
    const loom_low_schedule_table_t* schedule,
    const loom_low_schedule_dependency_index_t* dependency_index,
    const uint32_t* range_ends, uint32_t* path_cycles) {
  for (iree_host_size_t block_index = 0; block_index < schedule->block_count;
       ++block_index) {
    const loom_low_schedule_block_t* block = &schedule->blocks[block_index];
    for (uint32_t i = block->scheduled_node_count; i > 0; --i) {
      const uint32_t producer_node =
          schedule->scheduled_node_indices[block->scheduled_node_start + i - 1];
      const uint32_t group_begin =
          loom_low_schedule_dependency_index_group_begin(dependency_index,
                                                         producer_node);
      const uint32_t group_end = loom_low_schedule_dependency_index_group_end(
          dependency_index, producer_node);
      for (uint32_t group_index = group_begin; group_index < group_end;
           ++group_index) {
        const loom_low_schedule_dependency_group_t* group =
            loom_low_schedule_dependency_index_group_at(dependency_index,
                                                        group_index);
        const uint32_t consumer_node = group->consumer_node;
        // Monotone source paths remain entirely inside every suffix containing
        // their producer. Other paths are deliberately omitted from the proof.
        if (consumer_node <= producer_node ||
            consumer_node >= range_ends[producer_node]) {
          continue;
        }
        const uint32_t distance = loom_low_schedule_source_suffix_edge_distance(
            dependency_index, group_index);
        path_cycles[producer_node] = iree_max(
            path_cycles[producer_node],
            iree_math_saturating_add_u32(distance, path_cycles[consumer_node]));
      }
    }
  }
}

static void loom_low_schedule_source_suffix_finalize_bounds(
    const loom_low_schedule_table_t* schedule, uint32_t* bounds,
    const uint32_t* path_cycles) {
  for (iree_host_size_t block_index = 0; block_index < schedule->block_count;
       ++block_index) {
    const loom_low_schedule_block_t* block = &schedule->blocks[block_index];
    const uint32_t block_start = block->node_start;
    uint32_t range_end = block_start + block->node_count;
    bool has_following_range = false;
    uint32_t following_range_bound = 0;
    while (range_end > block_start) {
      uint32_t range_start = range_end - 1;
      while (range_start > block_start &&
             bounds[range_start - 1] == range_end) {
        --range_start;
      }
      uint32_t range_path_bound = 0;
      bool suffix_has_only_nonzero_width = true;
      for (uint32_t i = range_end; i > range_start; --i) {
        const uint32_t node_index = i - 1;
        range_path_bound = iree_max(range_path_bound, path_cycles[node_index]);
        suffix_has_only_nonzero_width &=
            !loom_low_schedule_source_suffix_node_has_zero_issue_width(
                &schedule->nodes[node_index]);
        uint32_t bound = range_path_bound;
        if (has_following_range) {
          bound = iree_math_saturating_add_u32(bound, following_range_bound);
          if (suffix_has_only_nonzero_width) {
            bound = iree_math_saturating_add_u32(bound, 1);
          }
        }
        bounds[node_index] = bound;
      }
      following_range_bound = bounds[range_start];
      has_following_range = true;
      range_end = range_start;
    }
  }
}

iree_status_t loom_low_schedule_source_suffix_bounds_build(
    const loom_low_schedule_table_t* schedule,
    const loom_low_schedule_dependency_index_t* dependency_index,
    iree_arena_allocator_t* scratch_arena, iree_arena_allocator_t* arena,
    const uint32_t** out_issue_cycle_lower_bounds) {
  *out_issue_cycle_lower_bounds = NULL;
  if (schedule->node_count == 0) {
    return iree_ok_status();
  }
  IREE_ASSERT_LE(schedule->node_count, UINT32_MAX);
  IREE_ASSERT_EQ(dependency_index->node_count, schedule->node_count);
  uint32_t* bounds = NULL;
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      arena, schedule->node_count, sizeof(*bounds), (void**)&bounds));
  uint32_t* path_cycles = NULL;
  IREE_RETURN_IF_ERROR(
      iree_arena_allocate_array(scratch_arena, schedule->node_count,
                                sizeof(*path_cycles), (void**)&path_cycles));
  memset(path_cycles, 0, schedule->node_count * sizeof(*path_cycles));

  loom_low_schedule_source_suffix_initialize_ranges(schedule, bounds);
  loom_low_schedule_source_suffix_compute_paths(schedule, dependency_index,
                                                bounds, path_cycles);
  loom_low_schedule_source_suffix_finalize_bounds(schedule, bounds,
                                                  path_cycles);
  *out_issue_cycle_lower_bounds = bounds;
  return iree_ok_status();
}
