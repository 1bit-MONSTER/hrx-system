// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_UTIL_SEGMENTED_STORAGE_TEST_UTIL_H_
#define LOOM_UTIL_SEGMENTED_STORAGE_TEST_UTIL_H_

#include <cstdint>

#include "iree/testing/gtest.h"
#include "loom/util/segmented_storage.h"

namespace loom::testing {

// One staged extension shape for rollback and retry coverage.
struct SegmentedStorageAppendCase {
  // Published segment prefix preceding the candidate extension.
  uint32_t prefix_count;
  // Proposed segment count, crossing one or more directory boundaries.
  uint32_t extended_count;
};

// Shared allocator and rollback fixture for segmented-storage tests.
class SegmentedStorageTest : public ::testing::Test {
 protected:
  // Runs every backing-allocation failure at the selected append boundaries.
  void CheckAppendFailuresAtBoundaries(const uint32_t* boundaries,
                                       iree_host_size_t boundary_count);

  // Runs every backing-allocation failure for the selected staged extensions.
  void CheckDiscardedExtensionCases(const SegmentedStorageAppendCase* cases,
                                    iree_host_size_t case_count);

  void ResetPool(iree_host_size_t block_size);

  // Number of backing allocations attempted since reset.
  iree_host_size_t allocation_count_ = 0;
  // Largest requested backing allocation since reset, in bytes.
  iree_host_size_t largest_allocation_ = 0;
  // Allocation ordinal to fail, or SIZE_MAX to allow every request.
  iree_host_size_t failure_index_ = SIZE_MAX;
  // Shared backing pool retained across arena resets.
  iree_arena_block_pool_t block_pool_ = {};
  // Arena owning every payload and directory page in one test.
  iree_arena_allocator_t arena_ = {};

 private:
  static iree_status_t Allocate(void* self, iree_allocator_command_t command,
                                const void* parameters, void** pointer);
  void InitializePool(iree_host_size_t block_size);
  void SetUp() override;
  void TearDown() override;
  void AppendRows(loom_segmented_storage_t* storage, uint32_t count);
  void ExpectRows(const loom_segmented_storage_t& storage, uint32_t count);
};

}  // namespace loom::testing

#endif  // LOOM_UTIL_SEGMENTED_STORAGE_TEST_UTIL_H_
