// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/target/arch/amd/xdna/aie2p/array/local_memory.h"

#include "iree/testing/gtest.h"

namespace {

const loom_xdna_tile_facts_t* ComputeFacts() {
  return loom_xdna_array_tile_kind_facts(loom_xdna_npu2_array_family(),
                                         LOOM_XDNA_TILE_KIND_COMPUTE);
}

TEST(Aie2pArrayLocalMemoryTest, WorkerProposalPacksFromHighestBank) {
  const loom_xdna_tile_facts_t* facts = ComputeFacts();
  uint32_t bank_cursors[4] = {0};
  uint8_t next_channel_bank = 2;
  loom_aie2p_array_local_memory_proposal_t proposal = {};

  ASSERT_TRUE(loom_aie2p_array_local_memory_propose_worker(
      facts, bank_cursors, /*byte_length=*/64, /*alignment=*/64, &proposal));
  EXPECT_EQ(proposal.owner_offset, 48u * 1024u);
  EXPECT_EQ(bank_cursors[3], 0u);

  loom_aie2p_array_local_memory_commit(facts, &proposal, bank_cursors,
                                       &next_channel_bank);
  EXPECT_EQ(bank_cursors[3], 64u);
  EXPECT_EQ(next_channel_bank, 2u);
}

TEST(Aie2pArrayLocalMemoryTest, WorkerProposalCanSelectExactBank) {
  const loom_xdna_tile_facts_t* facts = ComputeFacts();
  uint32_t bank_cursors[4] = {0};
  loom_aie2p_array_local_memory_proposal_t proposal = {};

  ASSERT_TRUE(loom_aie2p_array_local_memory_propose_worker_from_bank(
      facts, bank_cursors, /*first_bank=*/1, /*byte_length=*/64,
      /*alignment=*/64, &proposal));
  EXPECT_EQ(proposal.owner_offset, 16u * 1024u);
  EXPECT_EQ(proposal.first_bank, 1u);
  EXPECT_EQ(proposal.last_bank, 1u);
  EXPECT_FALSE(loom_aie2p_array_local_memory_propose_worker_from_bank(
      facts, bank_cursors, facts->memory.bank_count, /*byte_length=*/64,
      /*alignment=*/64, &proposal));
}

TEST(Aie2pArrayLocalMemoryTest, ChannelProposalAdvancesRoundRobinBank) {
  const loom_xdna_tile_facts_t* facts = ComputeFacts();
  uint32_t bank_cursors[4] = {0};
  uint8_t next_channel_bank = 0;
  loom_aie2p_array_local_memory_proposal_t proposal = {};

  ASSERT_TRUE(loom_aie2p_array_local_memory_propose_channel(
      facts, bank_cursors, next_channel_bank, /*byte_length=*/64,
      /*alignment=*/64, &proposal));
  EXPECT_EQ(proposal.owner_offset, 0u);
  loom_aie2p_array_local_memory_commit(facts, &proposal, bank_cursors,
                                       &next_channel_bank);
  EXPECT_EQ(bank_cursors[0], 64u);
  EXPECT_EQ(next_channel_bank, 1u);

  ASSERT_TRUE(loom_aie2p_array_local_memory_propose_channel(
      facts, bank_cursors, next_channel_bank, /*byte_length=*/64,
      /*alignment=*/64, &proposal));
  EXPECT_EQ(proposal.owner_offset, 16u * 1024u);
}

TEST(Aie2pArrayLocalMemoryTest, WideProposalRetainsCompleteBankTransition) {
  const loom_xdna_tile_facts_t* facts = ComputeFacts();
  uint32_t bank_cursors[4] = {0};
  uint8_t next_channel_bank = 0;
  loom_aie2p_array_local_memory_proposal_t proposal = {};

  ASSERT_TRUE(loom_aie2p_array_local_memory_propose_worker(
      facts, bank_cursors, facts->memory.local_capacity, /*alignment=*/64,
      &proposal));
  EXPECT_EQ(proposal.owner_offset, 0u);
  EXPECT_EQ(proposal.first_bank, 0u);
  EXPECT_EQ(proposal.last_bank, 3u);
  EXPECT_EQ(bank_cursors[0], 0u);

  loom_aie2p_array_local_memory_commit(facts, &proposal, bank_cursors,
                                       &next_channel_bank);
  for (uint32_t cursor : bank_cursors) {
    EXPECT_EQ(cursor, 16u * 1024u);
  }
}

TEST(Aie2pArrayLocalMemoryTest, RejectedProposalDoesNotMutateState) {
  const loom_xdna_tile_facts_t* facts = ComputeFacts();
  uint32_t bank_cursors[4] = {0};
  loom_aie2p_array_local_memory_proposal_t proposal = {};

  EXPECT_FALSE(loom_aie2p_array_local_memory_propose_worker(
      facts, bank_cursors, (uint64_t)facts->memory.local_capacity + 1u,
      /*alignment=*/64, &proposal));
  for (uint32_t cursor : bank_cursors) {
    EXPECT_EQ(cursor, 0u);
  }
}

}  // namespace
