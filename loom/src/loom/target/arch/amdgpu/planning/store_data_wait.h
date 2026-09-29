// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Short CDNA VMEM payload-retention windows over final physical packets.

#ifndef LOOM_TARGET_ARCH_AMDGPU_PLANNING_STORE_DATA_WAIT_H_
#define LOOM_TARGET_ARCH_AMDGPU_PLANNING_STORE_DATA_WAIT_H_

#include "loom/target/arch/amdgpu/planning/structural_packet.h"
#include "loom/target/arch/amdgpu/planning/wait_states.h"
#include "loom/target/arch/amdgpu/refs/target_refs.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct loom_amdgpu_store_data_wait_state_t
    loom_amdgpu_store_data_wait_state_t;

typedef struct loom_amdgpu_store_data_wait_match_t {
  // Local source node requiring the delay.
  uint32_t producer_node;
  // Original source-retention window for this writer.
  uint16_t required_cycles;
  // Intervening issue slots supplied by the native stream.
  uint16_t observed_cycles;
  // Remaining slots to insert before the packet.
  uint16_t cycles;
} loom_amdgpu_store_data_wait_match_t;

// Emits one ordered cross-block store-data wait state.
typedef iree_status_t (*loom_amdgpu_store_data_wait_emit_fn_t)(
    void* user_data, const loom_amdgpu_wait_state_t* wait_state);

// Allocates transient state, returning NULL on targets without this hazard.
iree_status_t loom_amdgpu_store_data_wait_create(
    const loom_low_schedule_table_t* schedule,
    const loom_low_allocation_table_t* allocation,
    iree_arena_allocator_t* arena,
    loom_amdgpu_store_data_wait_state_t** out_state);

// Starts local collection. Cross-block obligations are resolved from retained
// two-slot summaries after all blocks have been visited.
void loom_amdgpu_store_data_wait_begin_block(
    loom_amdgpu_store_data_wait_state_t* state, uint16_t block_index);

// Retains this packet's physical writes and matches local source windows.
// Structural information is supplied by the owning final-packet traversal.
loom_amdgpu_store_data_wait_match_t loom_amdgpu_store_data_wait_inspect(
    loom_amdgpu_store_data_wait_state_t* state,
    const loom_low_packet_view_t* packet,
    const loom_amdgpu_structural_packet_info_t* structural,
    loom_amdgpu_descriptor_traits_t traits);

// Accounts for inserted native wait instructions before the inspected packet.
void loom_amdgpu_store_data_wait_advance(
    loom_amdgpu_store_data_wait_state_t* state, uint64_t cycles);

// Publishes the inspected packet after any inserted fixed delays.
void loom_amdgpu_store_data_wait_commit(
    loom_amdgpu_store_data_wait_state_t* state,
    const loom_low_packet_view_t* packet);

// Retains local outgoing sources and saturated issue progress for this block.
void loom_amdgpu_store_data_wait_end_block(
    loom_amdgpu_store_data_wait_state_t* state);

// Resolves the CFG and emits cross-block delays in scheduled packet order.
iree_status_t loom_amdgpu_store_data_wait_resolve(
    loom_amdgpu_store_data_wait_state_t* state,
    loom_amdgpu_store_data_wait_emit_fn_t emit, void* emit_user_data);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TARGET_ARCH_AMDGPU_PLANNING_STORE_DATA_WAIT_H_
