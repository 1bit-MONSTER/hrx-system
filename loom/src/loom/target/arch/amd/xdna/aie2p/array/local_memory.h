// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Transactional AIE2P banked local-memory placement.

#ifndef LOOM_TARGET_ARCH_AMD_XDNA_AIE2P_ARRAY_LOCAL_MEMORY_H_
#define LOOM_TARGET_ARCH_AMD_XDNA_AIE2P_ARRAY_LOCAL_MEMORY_H_

#include "loom/target/arch/amd/xdna/array/facts.h"

#ifdef __cplusplus
extern "C" {
#endif

// Effects retained by one successful local-memory proposal.
typedef uint8_t loom_aie2p_array_local_memory_proposal_flags_t;
enum loom_aie2p_array_local_memory_proposal_flag_bits_e {
  // Advance the round-robin bank selected for the next channel ring.
  LOOM_AIE2P_ARRAY_LOCAL_MEMORY_PROPOSAL_FLAG_ADVANCE_CHANNEL_BANK = 1u << 0,
};

// Exact bank-cursor transition selected for one local-memory allocation.
typedef struct loom_aie2p_array_local_memory_proposal_t {
  // Byte offset in the tile owner's canonical local allocation space.
  uint32_t owner_offset;
  // Cursor following the allocation in last_bank.
  uint32_t ending_bank_cursor;
  // First bank whose cursor advances when the proposal is committed.
  uint8_t first_bank;
  // Last bank whose cursor advances when the proposal is committed.
  uint8_t last_bank;
  // Round-robin bank following a committed channel-ring allocation.
  uint8_t next_channel_bank;
  // Effects applied when the proposal is committed.
  loom_aie2p_array_local_memory_proposal_flags_t flags;
} loom_aie2p_array_local_memory_proposal_t;

// Selects one channel-ring allocation using the current round-robin bank.
// Returns false without mutating |bank_cursors| when no bank can contain the
// requested range. |out_proposal| is defined only when true is returned.
bool loom_aie2p_array_local_memory_propose_channel(
    const loom_xdna_tile_facts_t* facts, const uint32_t* bank_cursors,
    uint8_t next_channel_bank, uint64_t byte_length, uint64_t alignment,
    loom_aie2p_array_local_memory_proposal_t* out_proposal);

// Selects one persistent worker allocation beginning in |first_bank|. A wide
// allocation may span subsequent empty banks. Returns false without mutating
// |bank_cursors| when the requested range cannot begin in that bank.
bool loom_aie2p_array_local_memory_propose_worker_from_bank(
    const loom_xdna_tile_facts_t* facts, const uint32_t* bank_cursors,
    uint8_t first_bank, uint64_t byte_length, uint64_t alignment,
    loom_aie2p_array_local_memory_proposal_t* out_proposal);

// Selects one persistent worker allocation from the highest usable bank.
// Returns false without mutating |bank_cursors| when no bank can contain the
// requested range. |out_proposal| is defined only when true is returned.
bool loom_aie2p_array_local_memory_propose_worker(
    const loom_xdna_tile_facts_t* facts, const uint32_t* bank_cursors,
    uint64_t byte_length, uint64_t alignment,
    loom_aie2p_array_local_memory_proposal_t* out_proposal);

// Applies an allocation previously selected from the exact |bank_cursors|
// state. This is an infallible commit and performs no placement search.
void loom_aie2p_array_local_memory_commit(
    const loom_xdna_tile_facts_t* facts,
    const loom_aie2p_array_local_memory_proposal_t* proposal,
    uint32_t* bank_cursors, uint8_t* next_channel_bank);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TARGET_ARCH_AMD_XDNA_AIE2P_ARRAY_LOCAL_MEMORY_H_
