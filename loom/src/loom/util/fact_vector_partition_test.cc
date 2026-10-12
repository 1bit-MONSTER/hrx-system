// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/util/fact_vector_partition.h"

#include "iree/base/internal/arena.h"
#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"
#include "loom/util/fact_table.h"

namespace loom {
namespace {

class FactVectorPartitionTest : public ::testing::Test {
 protected:
  void SetUp() override {
    iree_arena_block_pool_initialize(4096, iree_allocator_system(),
                                     &block_pool_);
    iree_arena_initialize(&block_pool_, &arena_);
    IREE_ASSERT_OK(loom_value_fact_table_initialize(&table_, &arena_, 0));
  }
  void TearDown() override {
    iree_arena_deinitialize(&arena_);
    iree_arena_block_pool_deinitialize(&block_pool_);
  }

  // Backing storage shared by independently scoped fact tables.
  iree_arena_block_pool_t block_pool_;
  // Storage owning the primary fact table and its partitions.
  iree_arena_allocator_t arena_;
  // Primary scope under test.
  loom_value_fact_table_t table_ = {};
};

TEST_F(FactVectorPartitionTest, PartitionsOwnValuesAndTrackScope) {
  loom_value_id_t values[] = {1, 2, 3};
  IREE_ASSERT_OK(loom_value_fact_table_define_vector_partition(
      &table_, 7, {values, 3, 16}));
  const auto first = loom_value_fact_table_query_vector_partition(&table_, 7);
  ASSERT_EQ(first.count, 3u);
  EXPECT_NE(first.values, values);
  EXPECT_EQ(first.regular_lane_count, 16u);
  EXPECT_EQ(first.values[0], 1u);
  EXPECT_EQ(first.values[2], 3u);

  const auto bytes = arena_.used_allocation_size;
  IREE_ASSERT_OK(loom_value_fact_table_define_vector_partition(
      &table_, 7, {values, 3, 16}));
  EXPECT_EQ(loom_value_fact_table_query_vector_partition(&table_, 7).values,
            first.values);
  EXPECT_EQ(arena_.used_allocation_size, bytes);

  values[0] = 4;
  IREE_ASSERT_OK(loom_value_fact_table_define_vector_partition(&table_, 7,
                                                               {values, 2, 8}));
  const auto second = loom_value_fact_table_query_vector_partition(&table_, 7);
  ASSERT_EQ(second.count, 2u);
  EXPECT_EQ(second.regular_lane_count, 8u);
  EXPECT_EQ(second.values[0], 4u);
  EXPECT_EQ(first.values[0], 1u);

  loom_value_fact_table_clear_scope(&table_);
  EXPECT_EQ(table_.vector_partitions, nullptr);
  EXPECT_EQ(loom_value_fact_table_query_vector_partition(&table_, 7).count, 0u);
}

TEST_F(FactVectorPartitionTest, AliasesShareAndInvalidationWithdrawsFacts) {
  const loom_value_id_t values[] = {1, 2};
  IREE_ASSERT_OK(loom_value_fact_table_define_vector_partition(
      &table_, 7, {values, 2, 16}));
  IREE_ASSERT_OK(loom_value_fact_table_forward_vector_partition(&table_, 7, 8));
  EXPECT_EQ(loom_value_fact_table_query_vector_partition(&table_, 8).values,
            loom_value_fact_table_query_vector_partition(&table_, 7).values);

  loom_value_fact_table_undefine(&table_, 7);
  EXPECT_EQ(loom_value_fact_table_query_vector_partition(&table_, 7).count, 0u);
  EXPECT_EQ(loom_value_fact_table_query_vector_partition(&table_, 8).count, 2u);
  IREE_ASSERT_OK(loom_value_fact_table_forward_vector_partition(&table_, 7, 8));
  EXPECT_EQ(loom_value_fact_table_query_vector_partition(&table_, 8).count, 0u);
}

TEST_F(FactVectorPartitionTest, SameIdSnapshotsOwnPartitionValues) {
  iree_arena_allocator_t source_arena;
  iree_arena_initialize(&block_pool_, &source_arena);
  loom_value_fact_table_t source = {};
  IREE_ASSERT_OK(loom_value_fact_table_initialize(&source, &source_arena, 0));
  const loom_value_id_t values[] = {1, 2, 3};
  IREE_ASSERT_OK(loom_value_fact_table_define_vector_partition(
      &source, 7, {values, 3, 16}));
  IREE_ASSERT_OK(
      loom_value_fact_table_define(&source, 7, loom_value_facts_unknown()));
  const loom_value_id_t selected_values[] = {7};
  IREE_ASSERT_OK(loom_value_fact_table_clone_values(
      &table_, {&source, selected_values, 1}, nullptr));
  EXPECT_NE(loom_value_fact_table_query_vector_partition(&table_, 7).values,
            loom_value_fact_table_query_vector_partition(&source, 7).values);
  iree_arena_deinitialize(&source_arena);

  const auto copied = loom_value_fact_table_query_vector_partition(&table_, 7);
  ASSERT_EQ(copied.count, 3u);
  EXPECT_EQ(copied.regular_lane_count, 16u);
  EXPECT_EQ(copied.values[0], 1u);
  EXPECT_EQ(copied.values[2], 3u);
}

}  // namespace
}  // namespace loom
