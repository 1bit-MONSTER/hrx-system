// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/hal/file.h"

#include <array>

#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"

namespace {

using ::iree::testing::status::StatusIs;

// Each alignment can dominate the continuation stride independently. Small
// capacities exercise the same splitting as a native maximum request size.
class FileTransferTest
    : public ::testing::TestWithParam<iree_async_file_transfer_requirements_t> {
 protected:
  // Actual storage large enough for each prepared request.
  alignas(64) std::array<uint8_t, 256> storage_ = {};
};

TEST_P(FileTransferTest, SplitsWithoutLosingContinuationAlignment) {
  const auto requirements = GetParam();
  uint64_t offset = 128;
  size_t completed = 0;
  while (completed < storage_.size()) {
    size_t length = 0;
    IREE_ASSERT_OK(iree_hal_file_prepare_transfer(
        &requirements, offset, storage_.data() + completed,
        storage_.size() - completed, 63, &length));
    ASSERT_GT(length, 0u);
    ASSERT_LE(length, 63u);
    EXPECT_EQ(length, 32u);
    completed += length;
    offset += length;
  }
  EXPECT_EQ(completed, storage_.size());
}

TEST_P(FileTransferTest, ReusesAlignedStagingStorageForLargeLogicalRange) {
  const auto requirements = GetParam();
  const uint64_t remaining_length = (uint64_t{1} << 33) + 32;
  size_t length = 0;
  IREE_ASSERT_OK(iree_hal_file_prepare_transfer(
      &requirements, uint64_t{1} << 34, storage_.data(), remaining_length,
      storage_.size() - 1, &length));
  EXPECT_EQ(length, 224u);
  // The next chunk can reuse the same staging allocation; its logical file
  // offset, not the buffer's capacity, advances across the large file.
  IREE_ASSERT_OK(iree_hal_file_prepare_transfer(
      &requirements, (uint64_t{1} << 34) + length, storage_.data(),
      remaining_length - length, storage_.size() - 1, &length));
  EXPECT_EQ(length, 224u);
}

TEST_P(FileTransferTest, RejectsInsufficientCapacityWithoutProgress) {
  size_t length = 99;
  const auto requirements = GetParam();
  EXPECT_THAT(
      iree::Status(iree_hal_file_prepare_transfer(
          &requirements, 0, storage_.data(), storage_.size(), 31, &length)),
      StatusIs(iree::StatusCode::kResourceExhausted));
  EXPECT_EQ(length, 0u);
}

INSTANTIATE_TEST_SUITE_P(
    IndependentAlignment, FileTransferTest,
    ::testing::Values(iree_async_file_transfer_requirements_t{32, 8, 4},
                      iree_async_file_transfer_requirements_t{8, 32, 4},
                      iree_async_file_transfer_requirements_t{8, 4, 32}));

TEST(FileTransfer, FinalChunkOnlyNeedsRequestAlignment) {
  alignas(64) uint8_t storage[64];
  const iree_async_file_transfer_requirements_t requirements = {64, 16, 4};
  size_t length = 0;
  IREE_ASSERT_OK(iree_hal_file_prepare_transfer(&requirements, 16, storage, 12,
                                                sizeof(storage), &length));
  EXPECT_EQ(length, 12u);
}

TEST(FileTransfer, ChecksEveryContinuationConstraint) {
  alignas(64) uint8_t storage[64];
  const iree_async_file_transfer_requirements_t requirements = {16, 8, 4};
  size_t length = 0;
  IREE_ASSERT_OK(iree_hal_file_prepare_transfer(&requirements, 0, storage, 64,
                                                64, &length));
  EXPECT_EQ(length, 64u);
  for (size_t completed : {1, 4, 8}) {
    EXPECT_THAT(iree::Status(iree_hal_file_prepare_transfer(
                    &requirements, completed, storage + completed,
                    64 - completed, 64 - completed, &length)),
                StatusIs(iree::StatusCode::kInvalidArgument));
    EXPECT_EQ(length, 0u);
  }
  IREE_ASSERT_OK(iree_hal_file_prepare_transfer(&requirements, 16, storage + 16,
                                                48, 48, &length));
  EXPECT_EQ(length, 48u);
}

TEST(FileTransfer, EmptyAndByteGranularRequests) {
  uint8_t storage[7];
  const iree_async_file_transfer_requirements_t requirements = {1, 1, 1};
  size_t length = 99;
  IREE_ASSERT_OK(
      iree_hal_file_prepare_transfer(&requirements, 3, nullptr, 0, 0, &length));
  EXPECT_EQ(length, 0u);
  IREE_ASSERT_OK(iree_hal_file_prepare_transfer(&requirements, 3, storage + 1,
                                                6, 5, &length));
  EXPECT_EQ(length, 5u);
  EXPECT_THAT(iree::Status(iree_hal_file_prepare_transfer(
                  &requirements, 0, nullptr, 1, 1, &length)),
              StatusIs(iree::StatusCode::kInvalidArgument));
  EXPECT_EQ(length, 0u);
}

}  // namespace
