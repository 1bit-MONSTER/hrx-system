// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Physical register retention within XNACK replayable memory clauses.

#ifndef LOOM_TARGET_ARCH_AMDGPU_PLANNING_REPLAY_CLAUSE_H_
#define LOOM_TARGET_ARCH_AMDGPU_PLANNING_REPLAY_CLAUSE_H_

#include "iree/base/api.h"
#include "iree/base/internal/arena.h"
#include "loom/codegen/low/packet.h"
#include "loom/target/arch/amdgpu/refs/target_refs.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct loom_amdgpu_replay_clause_t loom_amdgpu_replay_clause_t;

// Allocates transient clause state only for selected replay modes and memory
// classes present in the function. Consumes the allocation's retained physical
// extents, including ABI-fixed registers. Returns NULL for unaffected
// functions.
iree_status_t loom_amdgpu_replay_clause_allocate(
    const loom_low_schedule_table_t* schedule,
    const loom_low_allocation_table_t* allocation,
    loom_amdgpu_descriptor_traits_t function_traits,
    iree_arena_allocator_t* arena, loom_amdgpu_replay_clause_t** out_state);

// Ends the clause at a concrete native instruction inserted by another owner.
// Elided operations and block labels do not end a clause.
void loom_amdgpu_replay_clause_break(loom_amdgpu_replay_clause_t* state);

// Consumes one packet after its other prefix waits have been planned. Returns
// the previous memory node when an additional non-memory instruction must
// precede this packet, or LOOM_LOW_PACKET_INDEX_NONE. On a required break, the
// retained clause already begins with this packet. The caller emits that break
// through its ordinary wait plan without breaking the new clause again.
uint32_t loom_amdgpu_replay_clause_apply(loom_amdgpu_replay_clause_t* state,
                                         const loom_low_packet_view_t* packet,
                                         loom_amdgpu_descriptor_traits_t traits,
                                         uint64_t instruction_count);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TARGET_ARCH_AMDGPU_PLANNING_REPLAY_CLAUSE_H_
