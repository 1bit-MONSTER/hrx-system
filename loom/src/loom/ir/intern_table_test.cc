// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/ir/intern_table.h"

#include <array>

#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"

namespace loom {
namespace {

class InternTableTest : public ::testing::Test {
 protected:
  void SetUp() override {
    iree_arena_block_pool_initialize(32 * 1024, iree_allocator_system(),
                                     &pool_);
    iree_arena_initialize(&pool_, &arena_);
  }

  void TearDown() override {
    iree_arena_deinitialize(&arena_);
    iree_arena_block_pool_deinitialize(&pool_);
  }

  static bool EqualIndex(const void* context, uint32_t index) {
    return *static_cast<const uint32_t*>(context) == index;
  }

  void Insert(loom_intern_table_t* table, uint32_t hash, uint32_t index) {
    const auto probe = loom_intern_table_probe(table, hash, EqualIndex, &index);
    ASSERT_EQ(probe.index, UINT32_MAX);
    const bool had_capacity = loom_intern_table_has_insert_capacity(table);
    auto slot = probe.slot;
    IREE_ASSERT_OK(loom_intern_table_reserve_insert(
        &arena_, table, hash, /*insertion_count=*/1, &slot));
    if (had_capacity) {
      EXPECT_EQ(slot, probe.slot);
    }
    ASSERT_EQ(loom_intern_table_const_bucket(table, slot)->index, UINT32_MAX);
    loom_intern_table_insert(table, slot, hash, index);
  }

  void ExpectIndex(const loom_intern_table_t& table, uint32_t hash,
                   uint32_t index) {
    const auto probe =
        loom_intern_table_probe(&table, hash, EqualIndex, &index);
    EXPECT_EQ(probe.index, index);
    EXPECT_EQ(loom_intern_table_const_bucket(&table, probe.slot)->hash, hash);
  }

  // Fixed blocks are shared by independent tables and recycled after reset.
  iree_arena_block_pool_t pool_ = {};
  // Sole owner of bucket chunks and pointer directory pages.
  iree_arena_allocator_t arena_ = {};
};

TEST_F(InternTableTest, LazyAllocationAndLoadThreshold) {
  loom_intern_table_t table;
  IREE_ASSERT_OK(loom_intern_table_initialize(&arena_, 0, &table));
  EXPECT_EQ(table.capacity, 0u);
  EXPECT_EQ(table.count, 0u);
  EXPECT_EQ(table.segments.segment_count, 0u);
  EXPECT_EQ(arena_.used_allocation_size, 0u);
  loom_intern_table_clear(&table);

  ASSERT_NO_FATAL_FAILURE(Insert(&table, 3, 0));
  ASSERT_EQ(table.capacity, 32u);
  EXPECT_EQ(table.segments.segment_count, 1u);
  EXPECT_EQ(arena_.used_allocation_size, sizeof(loom_intern_segment_t));
  for (uint32_t index = 1; index < 24; ++index) {
    ASSERT_NO_FATAL_FAILURE(Insert(&table, 3, index));
  }
  EXPECT_FALSE(loom_intern_table_has_insert_capacity(&table));
  const auto used = arena_.used_allocation_size;
  ASSERT_NO_FATAL_FAILURE(ExpectIndex(table, 3, 0));
  EXPECT_EQ(table.count, 24u);
  EXPECT_EQ(table.capacity, 32u);
  EXPECT_EQ(arena_.used_allocation_size, used);

  ASSERT_NO_FATAL_FAILURE(Insert(&table, 3, 24));
  EXPECT_EQ(table.capacity, 64u);
  EXPECT_EQ(arena_.used_allocation_size, used);
  EXPECT_EQ(loom_intern_table_capacity_for_entries(3), 4u);
  EXPECT_EQ(loom_intern_table_capacity_for_entries(4), 8u);
  EXPECT_EQ(loom_intern_table_capacity_for_entries(24), 32u);
  EXPECT_EQ(loom_intern_table_capacity_for_entries(25), 64u);
}

TEST_F(InternTableTest, SmallTableGrowthPreservesEveryHashOrder) {
  // Enumerate all four-entry hash sequences in the initial four-bucket domain.
  // The fourth insertion doubles the table after every possible cluster seam.
  for (uint32_t sequence = 0; sequence < 256; ++sequence) {
    SCOPED_TRACE(sequence);
    loom_intern_table_t table;
    IREE_ASSERT_OK(loom_intern_table_initialize(&arena_, 4, &table));
    for (uint32_t index = 0; index < 4; ++index) {
      ASSERT_NO_FATAL_FAILURE(
          Insert(&table, (sequence >> (index * 2)) & 3, index));
    }
    EXPECT_EQ(table.capacity, 8u);
    for (uint32_t index = 0; index < 4; ++index) {
      ASSERT_NO_FATAL_FAILURE(
          ExpectIndex(table, (sequence >> (index * 2)) & 3, index));
    }
    iree_arena_reset(&arena_);
  }
}

TEST_F(InternTableTest, ReserveMultipleKeysBeforePublication) {
  for (uint32_t insertion_count : {2u, 48u}) {
    SCOPED_TRACE(insertion_count);
    loom_intern_table_t table;
    IREE_ASSERT_OK(loom_intern_table_initialize(&arena_, 4, &table));
    // All keys collide across the end of the old table. The larger batch
    // requires more than one doubling without publishing intermediate rows.
    constexpr uint32_t kHash = 3;
    for (uint32_t index = 0; index < 3; ++index) {
      ASSERT_NO_FATAL_FAILURE(Insert(&table, kHash, index));
    }
    const uint32_t first_index = 3;
    const auto probe =
        loom_intern_table_probe(&table, kHash, EqualIndex, &first_index);
    auto slot = probe.slot;
    IREE_ASSERT_OK(loom_intern_table_reserve_insert(&arena_, &table, kHash,
                                                    insertion_count, &slot));
    EXPECT_EQ(table.count, first_index);
    EXPECT_GE(table.capacity * 3 / 4, first_index + insertion_count);
    const auto retained_bytes = arena_.used_allocation_size;
    for (uint32_t index = first_index; index < first_index + insertion_count;
         ++index) {
      EXPECT_TRUE(loom_intern_table_has_insert_capacity(&table));
      loom_intern_table_insert(&table, slot, kHash, index);
      slot = loom_intern_table_find_empty_slot(&table, kHash);
    }
    EXPECT_EQ(arena_.used_allocation_size, retained_bytes);
    for (uint32_t index = 0; index < first_index + insertion_count; ++index) {
      ASSERT_NO_FATAL_FAILURE(ExpectIndex(table, kHash, index));
    }
    iree_arena_reset(&arena_);
  }
}

TEST_F(InternTableTest, GrowthReusesChunksAndPreservesCollisionClusters) {
  constexpr uint32_t kEntryCount = 1537;
  constexpr uint32_t kFinalCapacity = 4096;
  for (uint32_t hash_mask : {UINT32_MAX, 0u, 7u, 63u}) {
    SCOPED_TRACE(hash_mask);
    // Decreasing home slots exercise wraparound and nonmonotone home order;
    // the zero mask forces full-hash collisions requiring equality checks.
    const auto hash = [hash_mask](uint32_t index) {
      return UINT32_MAX - ((index * 2654435761u) & hash_mask);
    };
    loom_intern_table_t table;
    IREE_ASSERT_OK(loom_intern_table_initialize(&arena_, 16, &table));
    std::array<const void*, kFinalCapacity / LOOM_INTERN_SEGMENT_CAPACITY>
        chunks = {};
    uint32_t retained_chunks = 0;
    for (uint32_t index = 0; index < kEntryCount; ++index) {
      const auto capacity = table.capacity;
      ASSERT_NO_FATAL_FAILURE(Insert(&table, hash(index), index));
      ASSERT_EQ(table.count, index + 1);
      if (table.capacity != capacity || index + 1 == kEntryCount) {
        for (uint32_t existing = 0; existing <= index; ++existing) {
          ASSERT_NO_FATAL_FAILURE(ExpectIndex(table, hash(existing), existing));
        }
      }
      ASSERT_LE(table.segments.segment_count, chunks.size());
      for (uint32_t i = 0; i < table.segments.segment_count; ++i) {
        const void* chunk =
            loom_segmented_storage_const_segment(&table.segments, i);
        if (i < retained_chunks) {
          EXPECT_EQ(chunk, chunks[i]);
        } else {
          chunks[i] = chunk;
        }
      }
      retained_chunks = table.segments.segment_count;
    }
    EXPECT_EQ(table.capacity, kFinalCapacity);
    EXPECT_EQ(table.segments.segment_count,
              table.capacity / LOOM_INTERN_SEGMENT_CAPACITY);
    // Only the current bucket capacity and its directory page are retained.
    const size_t directory_bytes =
        table.segments.segment_count >
                LOOM_SEGMENTED_STORAGE_INLINE_SEGMENT_COUNT
            ? sizeof(loom_segmented_storage_page_t)
            : 0;
    EXPECT_EQ(arena_.used_allocation_size,
              kFinalCapacity * sizeof(loom_intern_bucket_t) + directory_bytes);

    const auto used = arena_.used_allocation_size;
    loom_intern_table_clear(&table);
    EXPECT_EQ(table.count, 0u);
    EXPECT_EQ(table.capacity, kFinalCapacity);
    for (uint32_t index = 0; index < kEntryCount; ++index) {
      ASSERT_NO_FATAL_FAILURE(Insert(&table, hash(index), index));
    }
    EXPECT_EQ(arena_.used_allocation_size, used);
    for (uint32_t index = 0; index < kEntryCount; ++index) {
      ASSERT_NO_FATAL_FAILURE(ExpectIndex(table, hash(index), index));
    }
    iree_arena_reset(&arena_);
  }
}

TEST_F(InternTableTest, PreallocatedBucketsGrowIntoAPooledDirectory) {
  constexpr uint32_t kCapacity = LOOM_INTERN_SEGMENT_CAPACITY *
                                 LOOM_SEGMENTED_STORAGE_INLINE_SEGMENT_COUNT;
  loom_intern_table_t table;
  IREE_ASSERT_OK(loom_intern_table_initialize(&arena_, kCapacity, &table));
  EXPECT_EQ(table.segments.primary_page, nullptr);
  const auto* first_bucket = loom_intern_table_const_bucket(&table, 0);
  for (uint32_t index = 0; index <= kCapacity * 3 / 4; ++index) {
    ASSERT_NO_FATAL_FAILURE(Insert(&table, index * 2654435761u, index));
  }
  ASSERT_NE(table.segments.primary_page, nullptr);
  EXPECT_EQ(table.capacity, kCapacity * 2);
  EXPECT_EQ(loom_intern_table_const_bucket(&table, 0), first_bucket);
  EXPECT_EQ(arena_.used_allocation_size,
            table.capacity * sizeof(loom_intern_bucket_t) +
                sizeof(loom_segmented_storage_page_t));
  for (uint32_t index = 0; index <= kCapacity * 3 / 4; ++index) {
    ASSERT_NO_FATAL_FAILURE(ExpectIndex(table, index * 2654435761u, index));
  }
}

}  // namespace
}  // namespace loom
