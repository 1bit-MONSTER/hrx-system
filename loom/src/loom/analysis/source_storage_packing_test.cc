// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/analysis/source_storage_packing.h"

#include <stdint.h>

#include "iree/base/internal/arena.h"
#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"

namespace loom {
namespace {

struct NoninterferencePair {
  // First allocation whose lifetime is disjoint from the second.
  loom_value_id_t lhs_root_value_id;
  // Second allocation whose lifetime is disjoint from the first.
  loom_value_id_t rhs_root_value_id;
};

struct InterferenceQueryState {
  // Retained noninterference facts supplied by the test's allocation owner.
  NoninterferencePair noninterference_pairs[8];
  // Number of populated entries in noninterference_pairs.
  iree_host_size_t noninterference_pair_count = 0;
  // First root of a query that models a fallible analysis allocation.
  loom_value_id_t failure_lhs_root_value_id = LOOM_VALUE_ID_INVALID;
  // Second root of the fallible analysis query.
  loom_value_id_t failure_rhs_root_value_id = LOOM_VALUE_ID_INVALID;
};

static bool PairMatches(const NoninterferencePair& pair,
                        loom_value_id_t lhs_root_value_id,
                        loom_value_id_t rhs_root_value_id) {
  return (pair.lhs_root_value_id == lhs_root_value_id &&
          pair.rhs_root_value_id == rhs_root_value_id) ||
         (pair.lhs_root_value_id == rhs_root_value_id &&
          pair.rhs_root_value_id == lhs_root_value_id);
}

static iree_status_t QueryInterference(void* user_data,
                                       loom_value_id_t lhs_root_value_id,
                                       loom_value_id_t rhs_root_value_id,
                                       bool* out_interferes) {
  auto* state = static_cast<InterferenceQueryState*>(user_data);
  *out_interferes = true;
  const NoninterferencePair failure_pair = {
      state->failure_lhs_root_value_id,
      state->failure_rhs_root_value_id,
  };
  if (PairMatches(failure_pair, lhs_root_value_id, rhs_root_value_id)) {
    return iree_make_status(IREE_STATUS_ABORTED,
                            "injected interference query failure");
  }
  for (iree_host_size_t i = 0; i < state->noninterference_pair_count; ++i) {
    if (PairMatches(state->noninterference_pairs[i], lhs_root_value_id,
                    rhs_root_value_id)) {
      *out_interferes = false;
      break;
    }
  }
  return iree_ok_status();
}

class SourceStoragePackingTest : public ::testing::Test {
 protected:
  void SetUp() override {
    iree_arena_block_pool_initialize(4096, iree_allocator_system(),
                                     &block_pool_);
    iree_arena_initialize(&block_pool_, &arena_);
    IREE_ASSERT_OK(loom_source_storage_packing_allocate(
        loom_source_storage_packing_interference_callback_make(
            QueryInterference, &query_state_),
        nullptr, 0, &arena_, &packing_));
  }

  void TearDown() override {
    iree_arena_deinitialize(&arena_);
    iree_arena_block_pool_deinitialize(&block_pool_);
  }

  void AddNoninterference(loom_value_id_t lhs_root_value_id,
                          loom_value_id_t rhs_root_value_id) {
    ASSERT_LT(query_state_.noninterference_pair_count,
              IREE_ARRAYSIZE(query_state_.noninterference_pairs));
    query_state_
        .noninterference_pairs[query_state_.noninterference_pair_count++] = {
        lhs_root_value_id,
        rhs_root_value_id,
    };
  }

  uint64_t Append(loom_value_id_t root_value_id, uint64_t byte_length,
                  uint64_t byte_alignment) {
    uint64_t byte_offset = UINT64_MAX;
    IREE_CHECK_OK(loom_source_storage_packing_append(
        packing_, root_value_id, byte_length, byte_alignment, &byte_offset));
    return byte_offset;
  }

  InterferenceQueryState query_state_;
  iree_arena_block_pool_t block_pool_;
  iree_arena_allocator_t arena_;
  loom_source_storage_packing_t* packing_ = nullptr;
};

TEST_F(SourceStoragePackingTest, InterferingAllocationsPackWithAlignment) {
  EXPECT_EQ(Append(/*root_value_id=*/1, /*byte_length=*/12,
                   /*byte_alignment=*/4),
            0u);
  EXPECT_EQ(Append(/*root_value_id=*/2, /*byte_length=*/4,
                   /*byte_alignment=*/16),
            16u);

  const loom_source_storage_packing_requirement_t requirement =
      loom_source_storage_packing_requirement(packing_);
  EXPECT_EQ(requirement.byte_length, 20u);
  EXPECT_EQ(requirement.byte_alignment, 16u);
}

TEST_F(SourceStoragePackingTest, DisjointAllocationReusesSeveralRanges) {
  AddNoninterference(/*lhs_root_value_id=*/3, /*rhs_root_value_id=*/1);
  AddNoninterference(/*lhs_root_value_id=*/3, /*rhs_root_value_id=*/2);

  EXPECT_EQ(Append(/*root_value_id=*/1, /*byte_length=*/10240,
                   /*byte_alignment=*/16),
            0u);
  EXPECT_EQ(Append(/*root_value_id=*/2, /*byte_length=*/10240,
                   /*byte_alignment=*/16),
            10240u);
  EXPECT_EQ(Append(/*root_value_id=*/3, /*byte_length=*/32768,
                   /*byte_alignment=*/16),
            0u);

  const loom_source_storage_packing_requirement_t requirement =
      loom_source_storage_packing_requirement(packing_);
  EXPECT_EQ(requirement.byte_length, 32768u);
  EXPECT_EQ(requirement.byte_alignment, 16u);
}

TEST_F(SourceStoragePackingTest, AllocationPartiallyReusesOnePriorRange) {
  AddNoninterference(/*lhs_root_value_id=*/3, /*rhs_root_value_id=*/2);

  EXPECT_EQ(Append(/*root_value_id=*/1, /*byte_length=*/64,
                   /*byte_alignment=*/16),
            0u);
  EXPECT_EQ(Append(/*root_value_id=*/2, /*byte_length=*/64,
                   /*byte_alignment=*/16),
            64u);
  EXPECT_EQ(Append(/*root_value_id=*/3, /*byte_length=*/64,
                   /*byte_alignment=*/16),
            64u);

  const loom_source_storage_packing_requirement_t requirement =
      loom_source_storage_packing_requirement(packing_);
  EXPECT_EQ(requirement.byte_length, 128u);
  EXPECT_EQ(requirement.byte_alignment, 16u);
}

TEST_F(SourceStoragePackingTest, QueryFailureDoesNotPublishAllocation) {
  EXPECT_EQ(Append(/*root_value_id=*/1, /*byte_length=*/64,
                   /*byte_alignment=*/16),
            0u);
  query_state_.failure_lhs_root_value_id = 2;
  query_state_.failure_rhs_root_value_id = 1;
  uint64_t failed_offset = UINT64_MAX;
  iree_status_t status = loom_source_storage_packing_append(
      packing_, /*root_value_id=*/2, /*byte_length=*/64,
      /*byte_alignment=*/16, &failed_offset);
  EXPECT_EQ(iree_status_code(status), IREE_STATUS_ABORTED);
  iree_status_free(status);
  EXPECT_EQ(failed_offset, 0u);

  const loom_source_storage_packing_requirement_t failed_requirement =
      loom_source_storage_packing_requirement(packing_);
  EXPECT_EQ(failed_requirement.byte_length, 64u);
  EXPECT_EQ(failed_requirement.byte_alignment, 16u);

  query_state_.failure_lhs_root_value_id = LOOM_VALUE_ID_INVALID;
  query_state_.failure_rhs_root_value_id = LOOM_VALUE_ID_INVALID;
  EXPECT_EQ(Append(/*root_value_id=*/3, /*byte_length=*/64,
                   /*byte_alignment=*/16),
            64u);
}

TEST_F(SourceStoragePackingTest, ExtentOverflowDoesNotChangeRequirement) {
  EXPECT_EQ(Append(/*root_value_id=*/1, /*byte_length=*/INT64_MAX,
                   /*byte_alignment=*/1),
            0u);
  uint64_t failed_offset = UINT64_MAX;
  iree_status_t status = loom_source_storage_packing_append(
      packing_, /*root_value_id=*/2, /*byte_length=*/1,
      /*byte_alignment=*/1, &failed_offset);
  EXPECT_EQ(iree_status_code(status), IREE_STATUS_OUT_OF_RANGE);
  iree_status_free(status);
  EXPECT_EQ(failed_offset, 0u);

  const loom_source_storage_packing_requirement_t requirement =
      loom_source_storage_packing_requirement(packing_);
  EXPECT_EQ(requirement.byte_length, static_cast<uint64_t>(INT64_MAX));
  EXPECT_EQ(requirement.byte_alignment, 1u);
}

TEST_F(SourceStoragePackingTest, FixedRangesAndWorkerStorageShareOnePacking) {
  const loom_source_storage_packing_range_t fixed[] = {{64, 32}, {192, 16}};
  IREE_ASSERT_OK(loom_source_storage_packing_allocate(
      {}, fixed, IREE_ARRAYSIZE(fixed), &arena_, &packing_));
  EXPECT_EQ(Append(1, 80, 16), 96u);
  EXPECT_EQ(Append(2, 48, 16), 0u);

  uint64_t worker_offset = UINT64_MAX;
  IREE_ASSERT_OK(loom_source_storage_packing_reserve(packing_, 16, 16, nullptr,
                                                     0, &worker_offset));
  EXPECT_EQ(worker_offset, 48u);
  EXPECT_EQ(Append(3, 32, 16), 208u);
  EXPECT_EQ(loom_source_storage_packing_requirement(packing_).byte_length,
            240u);
}

TEST_F(SourceStoragePackingTest, FixedReservationsSurviveSourceLifetimeReuse) {
  const loom_source_storage_packing_range_t fixed[] = {{32, 16}};
  IREE_ASSERT_OK(loom_source_storage_packing_allocate(
      loom_source_storage_packing_interference_callback_make(QueryInterference,
                                                             &query_state_),
      fixed, IREE_ARRAYSIZE(fixed), &arena_, &packing_));
  AddNoninterference(2, 1);
  EXPECT_EQ(Append(1, 32, 16), 0u);
  EXPECT_EQ(Append(2, 64, 16), 48u);

  uint64_t worker_offset = UINT64_MAX;
  IREE_ASSERT_OK(loom_source_storage_packing_reserve(packing_, 48, 16, nullptr,
                                                     0, &worker_offset));
  EXPECT_EQ(worker_offset, 112u);
  AddNoninterference(3, 1);
  AddNoninterference(3, 2);
  EXPECT_EQ(Append(3, 80, 16), 160u);
}

TEST_F(SourceStoragePackingTest, FixedTailCountsWithoutOccupyingPrefix) {
  const loom_source_storage_packing_range_t fixed[] = {{960, 64}};
  IREE_ASSERT_OK(loom_source_storage_packing_allocate(
      {}, fixed, IREE_ARRAYSIZE(fixed), &arena_, &packing_));
  EXPECT_EQ(Append(1, 512, 64), 0u);
  EXPECT_EQ(loom_source_storage_packing_requirement(packing_).byte_length,
            1024u);
  uint64_t worker_offset = UINT64_MAX;
  IREE_ASSERT_OK(loom_source_storage_packing_reserve(packing_, 448, 64, nullptr,
                                                     0, &worker_offset));
  EXPECT_EQ(worker_offset, 512u);
  EXPECT_EQ(loom_source_storage_packing_requirement(packing_).byte_length,
            1024u);
  EXPECT_EQ(Append(2, 16, 16), 1024u);
}

TEST_F(SourceStoragePackingTest, EmptyAllocationsDoNotOccupyStorage) {
  const loom_source_storage_packing_range_t fixed[] = {{0, 64}};
  IREE_ASSERT_OK(loom_source_storage_packing_allocate(
      {}, fixed, IREE_ARRAYSIZE(fixed), &arena_, &packing_));
  EXPECT_EQ(Append(1, 0, 16), 0u);
  uint64_t empty_offset = UINT64_MAX;
  IREE_ASSERT_OK(loom_source_storage_packing_reserve(packing_, 0, 16, nullptr,
                                                     0, &empty_offset));
  EXPECT_EQ(empty_offset, 0u);
  EXPECT_EQ(Append(2, 16, 16), 64u);
}

TEST_F(SourceStoragePackingTest, ExclusionsApplyOnlyToTheirReservation) {
  EXPECT_EQ(Append(1, 32, 16), 0u);
  const loom_source_storage_packing_range_t excluded[] = {{16, 48}, {80, 16}};
  uint64_t offset = 0;
  IREE_ASSERT_OK(loom_source_storage_packing_reserve(
      packing_, 24, 16, excluded, IREE_ARRAYSIZE(excluded), &offset));
  EXPECT_EQ(offset, 96u);
  // The skipped bytes remain available to allocations with no conflict.
  IREE_ASSERT_OK(loom_source_storage_packing_reserve(packing_, 64, 16, nullptr,
                                                     0, &offset));
  EXPECT_EQ(offset, 32u);
  EXPECT_EQ(loom_source_storage_packing_requirement(packing_).byte_length,
            120u);
}

TEST_F(SourceStoragePackingTest, ReservationOverflowPreservesSourcePlacement) {
  EXPECT_EQ(Append(1, INT64_MAX, 1), 0u);
  uint64_t offset = UINT64_MAX;
  iree_status_t status =
      loom_source_storage_packing_reserve(packing_, 1, 1, nullptr, 0, &offset);
  EXPECT_EQ(iree_status_code(status), IREE_STATUS_OUT_OF_RANGE);
  iree_status_free(status);
  EXPECT_EQ(offset, 0u);
  EXPECT_EQ(loom_source_storage_packing_requirement(packing_).byte_length,
            static_cast<uint64_t>(INT64_MAX));
}

}  // namespace
}  // namespace loom
