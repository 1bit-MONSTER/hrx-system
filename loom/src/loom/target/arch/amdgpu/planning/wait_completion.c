// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/target/arch/amdgpu/planning/wait_completion.h"

void loom_amdgpu_wait_completion_analyze(
    const loom_low_schedule_table_t* schedule,
    const uint32_t* first_dependency_by_consumer,
    const loom_amdgpu_wait_dependency_t* dependencies,
    loom_amdgpu_wait_completion_node_t* nodes) {
  for (iree_host_size_t block_index = 0; block_index < schedule->block_count;
       ++block_index) {
    const loom_low_schedule_block_t* block = &schedule->blocks[block_index];
    uint32_t last_producers[LOOM_AMDGPU_WAIT_COUNTER_SLOT_COUNT];
    for (uint32_t slot = 0; slot < LOOM_AMDGPU_WAIT_COUNTER_SLOT_COUNT;
         ++slot) {
      last_producers[slot] = LOOM_LOW_SCHEDULE_NODE_NONE;
    }
    uint32_t workgroup_access_counter_mask = 0;
    for (uint32_t i = 0; i < block->scheduled_node_count; ++i) {
      const uint32_t node_index =
          schedule->scheduled_node_indices[block->scheduled_node_start + i];
      loom_amdgpu_wait_completion_node_t* node = &nodes[node_index];
      uint32_t reset_counter_mask =
          node->reset_counter_mask | (node->workgroup_barrier_counter_mask &
                                      workgroup_access_counter_mask);
      for (uint32_t dependency_index = first_dependency_by_consumer[node_index];
           dependency_index != LOOM_LOW_SCHEDULE_NODE_NONE;
           dependency_index = dependencies[dependency_index].next_dependency) {
        const loom_amdgpu_wait_dependency_t* dependency =
            &dependencies[dependency_index];
        const loom_low_schedule_node_t* producer =
            &schedule->nodes[dependency->producer_node];
        if (producer->block_index == block_index &&
            producer->scheduled_ordinal < i) {
          nodes[dependency->producer_node]
              .completed_before_block_exit_counter_mask |=
              dependency->counter_mask;
        }
        uint32_t counter_mask = dependency->counter_mask;
        while (counter_mask != 0) {
          const uint32_t slot =
              (uint32_t)iree_math_count_trailing_zeros_u32(counter_mask);
          if (last_producers[slot] == dependency->producer_node) {
            reset_counter_mask |= loom_amdgpu_wait_counter_mask_from_slot(slot);
          }
          counter_mask &= counter_mask - 1;
        }
      }
      node->reset_counter_mask = reset_counter_mask;
      for (uint32_t slot = 0; slot < LOOM_AMDGPU_WAIT_COUNTER_SLOT_COUNT;
           ++slot) {
        const uint32_t counter_mask =
            loom_amdgpu_wait_counter_mask_from_slot(slot);
        if ((reset_counter_mask & counter_mask) != 0) {
          last_producers[slot] = LOOM_LOW_SCHEDULE_NODE_NONE;
        }
        if ((node->producer_counter_mask & counter_mask) != 0) {
          last_producers[slot] = node_index;
        }
      }
      workgroup_access_counter_mask &= ~reset_counter_mask;
      workgroup_access_counter_mask |= node->workgroup_access_counter_mask;
    }
    uint32_t completed_counter_mask = 0;
    for (uint32_t i = block->scheduled_node_count; i > 0; --i) {
      const uint32_t node_index =
          schedule->scheduled_node_indices[block->scheduled_node_start + i - 1];
      loom_amdgpu_wait_completion_node_t* node = &nodes[node_index];
      completed_counter_mask |= node->completed_before_block_exit_counter_mask;
      node->completed_before_block_exit_counter_mask =
          completed_counter_mask & node->producer_counter_mask;
      // A reset precedes the current node's production. It completes earlier
      // producers without completing work newly issued by this node.
      completed_counter_mask |= node->reset_counter_mask;
    }
  }
}
