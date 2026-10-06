// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/target/arch/amd/xdna/aie2p/array/local_memory.h"

static bool loom_aie2p_array_local_memory_propose_in_bank(
    const loom_xdna_tile_facts_t* facts, const uint32_t* bank_cursors,
    uint8_t bank, uint64_t byte_length, uint64_t alignment,
    loom_aie2p_array_local_memory_proposal_t* out_proposal) {
  const uint32_t bank_capacity =
      facts->memory.local_capacity / facts->memory.bank_count;
  const uint32_t bank_begin = bank * bank_capacity;
  const uint32_t bank_end = bank_begin + bank_capacity;
  uint64_t owner_offset = bank_begin + bank_cursors[bank];
  if (!iree_checked_align_u64(owner_offset, alignment, &owner_offset)) {
    return false;
  }
  uint64_t owner_end = 0;
  if (!iree_checked_add_u64(owner_offset, byte_length, &owner_end)) {
    return false;
  }

  uint8_t last_bank = bank;
  if (owner_end > bank_end) {
    // Single-bank allocations retain stable bank affinity. Wider allocations
    // occupy every intervening bank prefix and cannot cross occupied storage.
    if (byte_length <= bank_capacity || owner_offset >= bank_end ||
        owner_end > facts->memory.local_capacity) {
      return false;
    }
    last_bank = (uint8_t)((owner_end - 1u) / bank_capacity);
    for (uint16_t i = (uint16_t)bank + 1u; i <= last_bank; ++i) {
      if (bank_cursors[i] != 0) {
        return false;
      }
    }
  }

  *out_proposal = (loom_aie2p_array_local_memory_proposal_t){
      .owner_offset = (uint32_t)owner_offset,
      .ending_bank_cursor =
          (uint32_t)owner_end - (uint32_t)last_bank * bank_capacity,
      .first_bank = bank,
      .last_bank = last_bank,
  };
  return true;
}

bool loom_aie2p_array_local_memory_propose_channel(
    const loom_xdna_tile_facts_t* facts, const uint32_t* bank_cursors,
    uint8_t next_channel_bank, uint64_t byte_length, uint64_t alignment,
    loom_aie2p_array_local_memory_proposal_t* out_proposal) {
  for (uint8_t attempt = 0; attempt < facts->memory.bank_count; ++attempt) {
    const uint8_t bank =
        (uint8_t)((next_channel_bank + attempt) % facts->memory.bank_count);
    if (!loom_aie2p_array_local_memory_propose_in_bank(
            facts, bank_cursors, bank, byte_length, alignment, out_proposal)) {
      continue;
    }
    out_proposal->next_channel_bank =
        (uint8_t)((bank + 1u) % facts->memory.bank_count);
    out_proposal->flags =
        LOOM_AIE2P_ARRAY_LOCAL_MEMORY_PROPOSAL_FLAG_ADVANCE_CHANNEL_BANK;
    return true;
  }
  return false;
}

bool loom_aie2p_array_local_memory_propose_worker_from_bank(
    const loom_xdna_tile_facts_t* facts, const uint32_t* bank_cursors,
    uint8_t first_bank, uint64_t byte_length, uint64_t alignment,
    loom_aie2p_array_local_memory_proposal_t* out_proposal) {
  return first_bank < facts->memory.bank_count &&
         loom_aie2p_array_local_memory_propose_in_bank(facts, bank_cursors,
                                                       first_bank, byte_length,
                                                       alignment, out_proposal);
}

bool loom_aie2p_array_local_memory_propose_worker(
    const loom_xdna_tile_facts_t* facts, const uint32_t* bank_cursors,
    uint64_t byte_length, uint64_t alignment,
    loom_aie2p_array_local_memory_proposal_t* out_proposal) {
  const uint8_t bank_count = facts->memory.bank_count;
  for (uint8_t attempt = 0; attempt < bank_count; ++attempt) {
    const uint8_t bank = (uint8_t)(bank_count - attempt - 1u);
    if (loom_aie2p_array_local_memory_propose_worker_from_bank(
            facts, bank_cursors, bank, byte_length, alignment, out_proposal)) {
      return true;
    }
  }
  return false;
}

void loom_aie2p_array_local_memory_commit(
    const loom_xdna_tile_facts_t* facts,
    const loom_aie2p_array_local_memory_proposal_t* proposal,
    uint32_t* bank_cursors, uint8_t* next_channel_bank) {
  const uint32_t bank_capacity =
      facts->memory.local_capacity / facts->memory.bank_count;
  for (uint16_t bank = proposal->first_bank; bank < proposal->last_bank;
       ++bank) {
    bank_cursors[bank] = bank_capacity;
  }
  bank_cursors[proposal->last_bank] = proposal->ending_bank_cursor;
  if (iree_any_bit_set(
          proposal->flags,
          LOOM_AIE2P_ARRAY_LOCAL_MEMORY_PROPOSAL_FLAG_ADVANCE_CHANNEL_BANK)) {
    *next_channel_bank = proposal->next_channel_bank;
  }
}
