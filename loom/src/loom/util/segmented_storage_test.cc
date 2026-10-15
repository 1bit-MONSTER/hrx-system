// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/util/segmented_storage.h"

#include <cstdint>

#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"
#include "loom/util/segmented_storage_test_util.h"

namespace loom {
namespace {

using testing::SegmentedStorageAppendCase;
using testing::SegmentedStorageTest;

TEST_F(SegmentedStorageTest, InlineAndPrimaryPagePointersStayStable) {
  loom_segmented_storage_t storage;
  loom_segmented_storage_initialize(sizeof(uint64_t), alignof(uint64_t),
                                    &storage);

  constexpr uint32_t kSegmentCount =
      LOOM_SEGMENTED_STORAGE_INLINE_SEGMENT_COUNT + 1;
  uint64_t* pointers[kSegmentCount];
  for (uint32_t i = 0; i < kSegmentCount; ++i) {
    void* segment = nullptr;
    IREE_ASSERT_OK(loom_segmented_storage_append(&storage, &arena_, &segment));
    pointers[i] = static_cast<uint64_t*>(segment);
    *pointers[i] = 0xCAFE0000u + i;
  }

  EXPECT_EQ(storage.segment_count, kSegmentCount);
  for (uint32_t i = 0; i < kSegmentCount; ++i) {
    EXPECT_EQ(loom_segmented_storage_segment(&storage, i), pointers[i]);
    EXPECT_EQ(*static_cast<const uint64_t*>(
                  loom_segmented_storage_const_segment(&storage, i)),
              0xCAFE0000u + i);
  }
}

TEST_F(SegmentedStorageTest, SecondaryPointerPage) {
  loom_segmented_storage_t storage;
  loom_segmented_storage_initialize(sizeof(uint32_t), alignof(uint32_t),
                                    &storage);

  constexpr uint32_t kSegmentCount =
      LOOM_SEGMENTED_STORAGE_SEGMENTS_PER_PAGE + 1;
  for (uint32_t i = 0; i < kSegmentCount; ++i) {
    void* segment = nullptr;
    IREE_ASSERT_OK(loom_segmented_storage_append(&storage, &arena_, &segment));
    *static_cast<uint32_t*>(segment) = i;
  }

  EXPECT_EQ(storage.segment_count, kSegmentCount);
  for (uint32_t i = 0; i < kSegmentCount; ++i) {
    EXPECT_EQ(*static_cast<const uint32_t*>(
                  loom_segmented_storage_const_segment(&storage, i)),
              i);
  }
}

TEST_F(SegmentedStorageTest, MultiplePageGroupsPreserveThePrefix) {
  loom_segmented_storage_t storage;
  loom_segmented_storage_initialize(sizeof(uint32_t), alignof(uint32_t),
                                    &storage);
  constexpr uint32_t kSegmentCount =
      LOOM_SEGMENTED_STORAGE_SEGMENTS_PER_PAGE *
          LOOM_SEGMENTED_STORAGE_SEGMENTS_PER_PAGE +
      1;
  for (uint32_t i = 0; i < kSegmentCount; ++i) {
    void* segment = nullptr;
    IREE_ASSERT_OK(loom_segmented_storage_append(&storage, &arena_, &segment));
    *static_cast<uint32_t*>(segment) = i;
  }
  for (uint32_t i = 0; i < kSegmentCount; ++i) {
    EXPECT_EQ(*static_cast<const uint32_t*>(
                  loom_segmented_storage_const_segment(&storage, i)),
              i);
  }
}

TEST_F(SegmentedStorageTest, MoveInlineDirectory) {
  loom_segmented_storage_t source;
  loom_segmented_storage_initialize(sizeof(uint32_t), alignof(uint32_t),
                                    &source);
  for (uint32_t i = 0; i < 8; ++i) {
    void* segment = nullptr;
    IREE_ASSERT_OK(loom_segmented_storage_append(&source, &arena_, &segment));
    *static_cast<uint32_t*>(segment) = i;
  }

  loom_segmented_storage_t storage;
  loom_segmented_storage_move(&source, &storage);

  EXPECT_EQ(source.segment_count, 0u);
  EXPECT_EQ(source.primary_page, nullptr);
  EXPECT_EQ(storage.primary_page, nullptr);
  for (uint32_t i = 0; i < storage.segment_count; ++i) {
    EXPECT_EQ(*static_cast<const uint32_t*>(
                  loom_segmented_storage_const_segment(&storage, i)),
              i);
  }
}

TEST_F(SegmentedStorageTest, InlineDirectoryCopyIsSelfContained) {
  loom_segmented_storage_t source;
  loom_segmented_storage_initialize(sizeof(uint32_t), alignof(uint32_t),
                                    &source);
  for (uint32_t i = 0; i < 8; ++i) {
    void* segment = nullptr;
    IREE_ASSERT_OK(loom_segmented_storage_append(&source, &arena_, &segment));
    *static_cast<uint32_t*>(segment) = i;
  }

  loom_segmented_storage_t copy = source;
  source.inline_segments[0] = nullptr;

  EXPECT_NE(loom_segmented_storage_const_segment(&copy, 0), nullptr);
  for (uint32_t i = 0; i < copy.segment_count; ++i) {
    EXPECT_EQ(*static_cast<const uint32_t*>(
                  loom_segmented_storage_const_segment(&copy, i)),
              i);
  }
}

TEST_F(SegmentedStorageTest, MoveExpandedDirectory) {
  loom_segmented_storage_t source;
  loom_segmented_storage_initialize(sizeof(uint32_t), alignof(uint32_t),
                                    &source);
  constexpr uint32_t kSegmentCount =
      LOOM_SEGMENTED_STORAGE_SEGMENTS_PER_PAGE + 1;
  for (uint32_t i = 0; i < kSegmentCount; ++i) {
    void* segment = nullptr;
    IREE_ASSERT_OK(loom_segmented_storage_append(&source, &arena_, &segment));
    *static_cast<uint32_t*>(segment) = i;
  }
  auto* source_primary_page = source.primary_page;

  loom_segmented_storage_t storage;
  loom_segmented_storage_move(&source, &storage);

  EXPECT_EQ(source.segment_count, 0u);
  EXPECT_EQ(storage.primary_page, source_primary_page);
  for (uint32_t i = 0; i < storage.segment_count; ++i) {
    EXPECT_EQ(*static_cast<const uint32_t*>(
                  loom_segmented_storage_const_segment(&storage, i)),
              i);
  }
}

TEST_F(SegmentedStorageTest, ReadOnlyCopiesSurviveDirectoryPromotion) {
  loom_segmented_storage_t storage;
  loom_segmented_storage_initialize(sizeof(uint32_t), alignof(uint32_t),
                                    &storage);
  const uint32_t copy_counts[] = {
      LOOM_SEGMENTED_STORAGE_INLINE_SEGMENT_COUNT,
      LOOM_SEGMENTED_STORAGE_INLINE_SEGMENT_COUNT + 1,
      LOOM_SEGMENTED_STORAGE_SEGMENTS_PER_PAGE + 1,
  };
  loom_segmented_storage_t copies[IREE_ARRAYSIZE(copy_counts)];
  uint32_t copy_index = 0;
  for (uint32_t i = 0; i < 2 * LOOM_SEGMENTED_STORAGE_SEGMENTS_PER_PAGE + 1;
       ++i) {
    void* segment = nullptr;
    IREE_ASSERT_OK(loom_segmented_storage_append(&storage, &arena_, &segment));
    *static_cast<uint32_t*>(segment) = i;
    if (copy_index < IREE_ARRAYSIZE(copy_counts) &&
        storage.segment_count == copy_counts[copy_index]) {
      copies[copy_index++] = storage;
    }
  }
  ASSERT_EQ(copy_index, IREE_ARRAYSIZE(copy_counts));
  for (const auto& copy : copies) {
    for (uint32_t i = 0; i < copy.segment_count; ++i) {
      EXPECT_EQ(loom_segmented_storage_const_segment(&copy, i),
                loom_segmented_storage_const_segment(&storage, i));
      EXPECT_EQ(*static_cast<const uint32_t*>(
                    loom_segmented_storage_const_segment(&copy, i)),
                i);
    }
  }
}

TEST_F(SegmentedStorageTest, OveralignedPayloads) {
  loom_segmented_storage_t storage;
  loom_segmented_storage_initialize(/*segment_size=*/192,
                                    /*segment_alignment=*/256, &storage);

  for (uint32_t i = 0; i < 64; ++i) {
    void* segment = nullptr;
    IREE_ASSERT_OK(loom_segmented_storage_append(&storage, &arena_, &segment));
    EXPECT_EQ(reinterpret_cast<uintptr_t>(segment) % 256, 0u);
  }
}

TEST_F(SegmentedStorageTest, ArenaResetReusesPoolBlocks) {
  loom_segmented_storage_t storage;
  loom_segmented_storage_initialize(/*segment_size=*/4096,
                                    /*segment_alignment=*/64, &storage);
  for (uint32_t i = 0; i < 32; ++i) {
    void* segment = nullptr;
    IREE_ASSERT_OK(loom_segmented_storage_append(&storage, &arena_, &segment));
  }

  iree_arena_block_pool_statistics_t warm_statistics;
  iree_arena_block_pool_query_statistics(&block_pool_, &warm_statistics);
  const iree_host_size_t warm_allocation_count = allocation_count_;
  ASSERT_GT(warm_allocation_count, 0u);
  iree_arena_reset(&arena_);
  loom_segmented_storage_initialize(/*segment_size=*/4096,
                                    /*segment_alignment=*/64, &storage);
  for (uint32_t i = 0; i < 32; ++i) {
    void* segment = nullptr;
    IREE_ASSERT_OK(loom_segmented_storage_append(&storage, &arena_, &segment));
  }

  iree_arena_block_pool_statistics_t reused_statistics;
  iree_arena_block_pool_query_statistics(&block_pool_, &reused_statistics);
  EXPECT_EQ(reused_statistics.block_system_allocation_count,
            warm_statistics.block_system_allocation_count);
  EXPECT_EQ(reused_statistics.oversized_allocation_count,
            warm_statistics.oversized_allocation_count);
  EXPECT_EQ(allocation_count_, warm_allocation_count);
}

TEST_F(SegmentedStorageTest, NormalPoolSizesReuseDirectoryAndPayloadStorage) {
  for (const iree_host_size_t block_size : {32 * 1024u, 128 * 1024u}) {
    SCOPED_TRACE(block_size);
    ResetPool(block_size);
    iree_host_size_t warm_allocation_count = 0;
    for (uint32_t iteration = 0; iteration < 3; ++iteration) {
      loom_segmented_storage_t storage;
      loom_segmented_storage_initialize(sizeof(uint64_t), alignof(uint64_t),
                                        &storage);
      for (uint32_t i = 0; i < LOOM_SEGMENTED_STORAGE_SEGMENTS_PER_PAGE + 1;
           ++i) {
        void* segment = nullptr;
        IREE_ASSERT_OK(
            loom_segmented_storage_append(&storage, &arena_, &segment));
        *static_cast<uint64_t*>(segment) = i;
      }
      if (iteration == 0) {
        warm_allocation_count = allocation_count_;
        ASSERT_GT(warm_allocation_count, 0u);
      }
      EXPECT_EQ(allocation_count_, warm_allocation_count);
      EXPECT_LE(largest_allocation_, block_size);
      iree_arena_reset(&arena_);
    }
  }
}

TEST_F(SegmentedStorageTest, RepresentativeAppendFailuresCanRewindAndRetry) {
  const uint32_t boundaries[] = {
      0,
      LOOM_SEGMENTED_STORAGE_INLINE_SEGMENT_COUNT,
      LOOM_SEGMENTED_STORAGE_SEGMENTS_PER_PAGE,
  };
  CheckAppendFailuresAtBoundaries(boundaries, IREE_ARRAYSIZE(boundaries));
}

TEST_F(SegmentedStorageTest,
       RepresentativeDiscardedExtensionsPreservePrefixAndRetry) {
  constexpr uint32_t kInline = LOOM_SEGMENTED_STORAGE_INLINE_SEGMENT_COUNT;
  constexpr uint32_t kPage = LOOM_SEGMENTED_STORAGE_SEGMENTS_PER_PAGE;
  const SegmentedStorageAppendCase cases[] = {
      {0, 4},
      {kInline - 1, kInline + 3},
      {kPage - 1, kPage + 3},
  };
  CheckDiscardedExtensionCases(cases, IREE_ARRAYSIZE(cases));
}

}  // namespace
}  // namespace loom
