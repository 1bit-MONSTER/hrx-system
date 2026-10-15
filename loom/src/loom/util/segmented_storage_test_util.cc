// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/util/segmented_storage_test_util.h"

#include "iree/testing/status_matchers.h"

namespace loom::testing {

iree_status_t SegmentedStorageTest::Allocate(void* self,
                                             iree_allocator_command_t command,
                                             const void* parameters,
                                             void** pointer) {
  auto* test = static_cast<SegmentedStorageTest*>(self);
  if (command != IREE_ALLOCATOR_COMMAND_FREE) {
    const auto* allocation =
        static_cast<const iree_allocator_alloc_params_t*>(parameters);
    test->largest_allocation_ =
        iree_max(test->largest_allocation_, allocation->byte_length);
    if (test->allocation_count_++ == test->failure_index_) {
      return iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                              "injected backing allocation failure");
    }
  }
  const auto allocator = iree_allocator_system();
  return allocator.ctl(allocator.self, command, parameters, pointer);
}

void SegmentedStorageTest::InitializePool(iree_host_size_t block_size) {
  iree_arena_block_pool_initialize(block_size, {this, Allocate}, &block_pool_);
  iree_arena_initialize(&block_pool_, &arena_);
}

void SegmentedStorageTest::SetUp() { InitializePool(128 * 1024); }

void SegmentedStorageTest::ResetPool(iree_host_size_t block_size) {
  iree_arena_deinitialize(&arena_);
  iree_arena_block_pool_deinitialize(&block_pool_);
  allocation_count_ = 0;
  largest_allocation_ = 0;
  failure_index_ = SIZE_MAX;
  InitializePool(block_size);
}

void SegmentedStorageTest::TearDown() {
  iree_arena_deinitialize(&arena_);
  iree_arena_block_pool_deinitialize(&block_pool_);
}

void SegmentedStorageTest::AppendRows(loom_segmented_storage_t* storage,
                                      uint32_t count) {
  for (uint32_t i = storage->segment_count; i < count; ++i) {
    void* segment = nullptr;
    IREE_ASSERT_OK(loom_segmented_storage_append(storage, &arena_, &segment));
    *static_cast<uint32_t*>(segment) = i;
  }
}

void SegmentedStorageTest::ExpectRows(const loom_segmented_storage_t& storage,
                                      uint32_t count) {
  ASSERT_EQ(storage.segment_count, count);
  for (uint32_t i = 0; i < count; ++i) {
    EXPECT_EQ(*static_cast<const uint32_t*>(
                  loom_segmented_storage_const_segment(&storage, i)),
              i);
  }
}

void SegmentedStorageTest::CheckAppendFailuresAtBoundaries(
    const uint32_t* boundaries, iree_host_size_t boundary_count) {
  for (iree_host_size_t boundary_index = 0; boundary_index < boundary_count;
       ++boundary_index) {
    const uint32_t boundary = boundaries[boundary_index];
    SCOPED_TRACE(boundary);
    for (iree_host_size_t failure_index = 0;; ++failure_index) {
      SCOPED_TRACE(failure_index);
      // Valid small blocks expose each directory allocation to the backing
      // allocator. Each attempt starts without blocks from an earlier retry.
      ResetPool(128);
      loom_segmented_storage_t storage;
      loom_segmented_storage_initialize(sizeof(uint32_t), alignof(uint32_t),
                                        &storage);
      ASSERT_NO_FATAL_FAILURE(AppendRows(&storage, boundary));
      // Fill the current allocation block so the payload allocation itself,
      // as well as later directory allocations, has a fallible backing call.
      void* arena_tail = nullptr;
      IREE_ASSERT_OK(iree_arena_allocate(
          &arena_, iree_arena_block_pool_max_allocation_size(&block_pool_),
          &arena_tail));
      const loom_segmented_storage_t before = storage;
      const auto checkpoint = iree_arena_checkpoint_save(&arena_);
      allocation_count_ = 0;
      failure_index_ = failure_index;
      void* segment = nullptr;
      iree_status_t status =
          loom_segmented_storage_append(&storage, &arena_, &segment);
      failure_index_ = SIZE_MAX;
      if (iree_status_is_ok(status)) {
        EXPECT_LE(allocation_count_, failure_index);
        EXPECT_EQ(storage.segment_count, boundary + 1);
        break;
      }
      IREE_EXPECT_STATUS_IS(IREE_STATUS_RESOURCE_EXHAUSTED, status);
      EXPECT_EQ(allocation_count_, failure_index + 1);
      EXPECT_EQ(segment, nullptr);
      EXPECT_EQ(storage.segment_count, before.segment_count);
      EXPECT_EQ(storage.segment_size, before.segment_size);
      EXPECT_EQ(storage.segment_alignment, before.segment_alignment);
      EXPECT_EQ(storage.primary_page, before.primary_page);
      EXPECT_EQ(storage.page_directory, before.page_directory);
      for (uint32_t i = 0; i < LOOM_SEGMENTED_STORAGE_INLINE_SEGMENT_COUNT;
           ++i) {
        EXPECT_EQ(storage.inline_segments[i], before.inline_segments[i]);
      }
      iree_arena_checkpoint_restore(&checkpoint);
      IREE_ASSERT_OK(
          loom_segmented_storage_append(&storage, &arena_, &segment));
      *static_cast<uint32_t*>(segment) = boundary;
      ExpectRows(storage, boundary + 1);
    }
  }
}

void SegmentedStorageTest::CheckDiscardedExtensionCases(
    const SegmentedStorageAppendCase* cases, iree_host_size_t case_count) {
  uint32_t partial_failures = 0;
  for (iree_host_size_t case_index = 0; case_index < case_count; ++case_index) {
    const auto& test = cases[case_index];
    SCOPED_TRACE(test.prefix_count);
    ResetPool(128);
    loom_segmented_storage_t storage;
    loom_segmented_storage_initialize(sizeof(uint32_t), alignof(uint32_t),
                                      &storage);
    ASSERT_NO_FATAL_FAILURE(AppendRows(&storage, test.prefix_count));
    void* arena_tail = nullptr;
    IREE_ASSERT_OK(iree_arena_allocate(
        &arena_, iree_arena_block_pool_max_allocation_size(&block_pool_),
        &arena_tail));
    const auto checkpoint = iree_arena_checkpoint_save(&arena_);
    const auto used_bytes = arena_.used_allocation_size;
    const auto owned_bytes = arena_.total_allocation_size;
    const loom_segmented_storage_t prefix = storage;
    for (iree_host_size_t failure_index = 0;; ++failure_index) {
      SCOPED_TRACE(failure_index);
      loom_segmented_storage_t staged = storage;
      allocation_count_ = 0;
      failure_index_ = failure_index;
      iree_status_t status = iree_ok_status();
      for (uint32_t i = test.prefix_count;
           i < test.extended_count && iree_status_is_ok(status); ++i) {
        void* segment = nullptr;
        status = loom_segmented_storage_append(&staged, &arena_, &segment);
        if (iree_status_is_ok(status)) {
          *static_cast<uint32_t*>(segment) = i;
        }
      }
      failure_index_ = SIZE_MAX;
      const bool completed = iree_status_is_ok(status);
      if (completed) {
        EXPECT_LE(allocation_count_, failure_index);
      } else {
        IREE_EXPECT_STATUS_IS(IREE_STATUS_RESOURCE_EXHAUSTED, status);
        EXPECT_EQ(allocation_count_, failure_index + 1);
        partial_failures += staged.segment_count > test.prefix_count;
      }
      ASSERT_NO_FATAL_FAILURE(ExpectRows(storage, test.prefix_count));

      // Discard both partial and complete extensions. Trimming returned
      // blocks exercises stale unpublished slots before the original prefix
      // becomes the sole append owner again.
      iree_arena_checkpoint_restore(&checkpoint);
      EXPECT_EQ(arena_.used_allocation_size, used_bytes);
      EXPECT_EQ(arena_.total_allocation_size, owned_bytes);
      iree_arena_block_pool_trim(&block_pool_);
      ASSERT_NO_FATAL_FAILURE(ExpectRows(storage, test.prefix_count));
      ASSERT_NO_FATAL_FAILURE(AppendRows(&storage, test.extended_count));
      ASSERT_NO_FATAL_FAILURE(ExpectRows(storage, test.extended_count));
      if (completed) {
        break;
      }
      // Keep the same live prefix across attempts while returning every
      // extension block, so each failure ordinal has fresh backing calls.
      iree_arena_checkpoint_restore(&checkpoint);
      storage = prefix;
      iree_arena_block_pool_trim(&block_pool_);
    }
  }
  EXPECT_GT(partial_failures, 0u);
}

}  // namespace loom::testing
