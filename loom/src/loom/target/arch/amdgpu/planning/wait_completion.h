// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Counter progress and completion requirements shared by wait frontiers.

#ifndef LOOM_TARGET_ARCH_AMDGPU_PLANNING_WAIT_COMPLETION_H_
#define LOOM_TARGET_ARCH_AMDGPU_PLANNING_WAIT_COMPLETION_H_

#include "loom/codegen/low/schedule/types.h"
#include "loom/target/arch/amdgpu/planning/wait_counters.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum loom_amdgpu_wait_dependency_flag_bits_e {
  // The dependency is an SSA use eligible for canonical-loop relocation.
  LOOM_AMDGPU_WAIT_DEPENDENCY_FLAG_SSA_USE = 1u << 0,
} loom_amdgpu_wait_dependency_flag_bits_t;
typedef uint8_t loom_amdgpu_wait_dependency_flags_t;

// One target-counter dependency retained by AMDGPU wait planning.
typedef struct loom_amdgpu_wait_dependency_t {
  // Producer node for the dependency.
  uint32_t producer_node;
  // Consumer node for the dependency.
  uint32_t consumer_node;
  // Next dependency for the same consumer or relocated loop-entry slot.
  uint32_t next_dependency;
  // Counters produced by |producer_node| and needed by this use.
  uint32_t counter_mask;
  // Target wait-plan reason identifier preserved for action provenance.
  uint16_t reason_id;
  // Dependency classification flags.
  loom_amdgpu_wait_dependency_flags_t flags;
} loom_amdgpu_wait_dependency_t;

// Per-node counter facts consumed by loop and incoming-storage frontiers.
typedef struct loom_amdgpu_wait_completion_node_t {
  // Counters advanced when this node executes.
  loom_amdgpu_wait_counter_mask_t producer_counter_mask;
  // Counters advanced by writes when this node executes.
  loom_amdgpu_wait_counter_mask_t write_counter_mask;
  // Counters guaranteed complete before this node produces new work. Includes
  // explicit/implicit resets and locally proven completion requirements.
  loom_amdgpu_wait_counter_mask_t reset_counter_mask;
  // This node's producer counters guaranteed complete before block exit.
  loom_amdgpu_wait_counter_mask_t completed_before_block_exit_counter_mask;
  // Counter domains in which the node can create a target hazard.
  loom_amdgpu_wait_counter_mask_t hazard_counter_mask;
  // Counter classes advanced by workgroup-memory accesses.
  loom_amdgpu_wait_counter_mask_t workgroup_access_counter_mask;
  // Workgroup-memory access counters observed by this node's barrier.
  loom_amdgpu_wait_counter_mask_t workgroup_barrier_counter_mask;
} loom_amdgpu_wait_completion_node_t;

static_assert(sizeof(loom_amdgpu_wait_completion_node_t) == 7,
              "wait completion facts must remain compact");

// Records guaranteed local completion in |nodes| from the schedule's retained
// counter dependencies. Requiring a local producer retires its ordered prefix;
// requiring the latest producer also resets the counter epoch. Construction
// takes linear work with fixed counter-sized scratch and no allocation.
// Consumers retain these facts instead of deriving completion again.
void loom_amdgpu_wait_completion_analyze(
    const loom_low_schedule_table_t* schedule,
    const uint32_t* first_dependency_by_consumer,
    const loom_amdgpu_wait_dependency_t* dependencies,
    loom_amdgpu_wait_completion_node_t* nodes);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TARGET_ARCH_AMDGPU_PLANNING_WAIT_COMPLETION_H_
