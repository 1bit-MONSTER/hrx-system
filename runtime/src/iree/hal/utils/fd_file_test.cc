// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/hal/utils/fd_file.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>

#include "iree/io/file_contents.h"
#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"
#include "iree/testing/temp_file.h"

#if IREE_FILE_IO_ENABLE

namespace {

using ::iree::testing::status::StatusIs;

struct AlignedDeleter {
  void operator()(uint8_t* pointer) const {
    iree_allocator_free_aligned(iree_allocator_system(), pointer);
  }
};

class FdFileTest : public ::testing::TestWithParam<iree_io_file_mode_t> {
 protected:
  void SetUp() override {
    expected_.resize(256 * 1024);
    for (size_t i = 0; i < expected_.size(); ++i) {
      expected_[i] = uint8_t(i * 17 + 5);
    }
    path_ = iree::testing::TempFilePath("iree_hal_fd_file");
    IREE_ASSERT_OK(iree_io_file_contents_write(
        path_.path_view(),
        iree_make_const_byte_span(expected_.data(), expected_.size()),
        iree_allocator_system()));
    iree_io_file_handle_t* handle = nullptr;
    IREE_ASSERT_OK(iree_io_file_handle_open(
        IREE_IO_FILE_MODE_READ | IREE_IO_FILE_MODE_WRITE | GetParam(),
        path_.path_view(), iree_allocator_system(), &handle));
    iree::Status status = iree_hal_fd_file_from_handle(
        IREE_HAL_MEMORY_ACCESS_READ | IREE_HAL_MEMORY_ACCESS_WRITE, handle,
        /*proactor=*/nullptr, iree_allocator_system(), &file_);
    iree_io_file_handle_release(handle);
    if (status.code() == iree::StatusCode::kUnavailable &&
        iree_any_bit_set(GetParam(), IREE_IO_FILE_MODE_DIRECT)) {
      ASSERT_EQ(file_, nullptr);
      GTEST_SKIP() << status.ToString();
    }
    IREE_ASSERT_OK(status);
  }

  void TearDown() override {
    iree_hal_file_release(file_);
    if (path_) {
      EXPECT_TRUE(path_.Remove());
    }
  }

  // Native file and retained expected bytes for independent readback.
  iree::testing::TempFilePath path_;
  // File owner, including the reference retained after the input handle closes.
  iree_hal_file_t* file_ = nullptr;
  // Complete expected contents, including bytes outside the changed extent.
  std::vector<uint8_t> expected_;
};

TEST_P(FdFileTest, RetainsRequirementsAndPublishesChangedRange) {
  const auto* requirements = iree_hal_file_requirements(file_);
  const auto& read = requirements->read;
  const auto& write = requirements->write;
  const size_t alignment =
      std::max(read.buffer_alignment, write.buffer_alignment);
  const uint64_t stride = std::max(
      {read.offset_alignment, write.offset_alignment,
       uint64_t(read.length_alignment), uint64_t(write.length_alignment)});
  ASSERT_LE(alignment, 64 * 1024u);
  ASSERT_LE(stride, 64 * 1024u);
  ASSERT_TRUE(iree_host_size_is_power_of_two(alignment));
  ASSERT_TRUE(iree_host_size_is_power_of_two(stride));
  const size_t length = iree_host_align(37, stride);
  const uint64_t offset = stride;
  void* allocation = nullptr;
  IREE_ASSERT_OK(iree_allocator_malloc_aligned(iree_allocator_system(),
                                               length + 2 * alignment,
                                               alignment, 0, &allocation));
  std::unique_ptr<uint8_t, AlignedDeleter> owner(
      static_cast<uint8_t*>(allocation));
  uint8_t* buffer = owner.get() + alignment;
  IREE_ASSERT_OK(
      iree_hal_file_read(file_, offset, iree_make_byte_span(buffer, length)));
  EXPECT_EQ(std::memcmp(buffer, expected_.data() + offset, length), 0);

  for (size_t i = 0; i < length; ++i) {
    buffer[i] = uint8_t(i * 31 + 73);
  }
  if (write.buffer_alignment > 1) {
    EXPECT_THAT(
        iree::Status(iree_hal_file_write(
            file_, offset, iree_make_const_byte_span(buffer + 1, length))),
        StatusIs(iree::StatusCode::kInvalidArgument));
  }
  if (write.offset_alignment > 1) {
    EXPECT_THAT(
        iree::Status(iree_hal_file_write(
            file_, offset + 1, iree_make_const_byte_span(buffer, length))),
        StatusIs(iree::StatusCode::kInvalidArgument));
  }
  if (write.length_alignment > 1) {
    EXPECT_THAT(
        iree::Status(iree_hal_file_write(
            file_, offset, iree_make_const_byte_span(buffer, length - 1))),
        StatusIs(iree::StatusCode::kInvalidArgument));
  }
  // Failed requests must not have changed any bytes in the file.
  std::vector<uint8_t> replacement(buffer, buffer + length);
  IREE_ASSERT_OK(
      iree_hal_file_read(file_, offset, iree_make_byte_span(buffer, length)));
  EXPECT_EQ(std::memcmp(buffer, expected_.data() + offset, length), 0);
  std::memcpy(buffer, replacement.data(), length);
  IREE_ASSERT_OK(iree_hal_file_write(
      file_, offset, iree_make_const_byte_span(buffer, length)));
  std::memcpy(expected_.data() + offset, buffer, length);
  std::memset(buffer, 0, length);
  IREE_ASSERT_OK(
      iree_hal_file_read(file_, offset, iree_make_byte_span(buffer, length)));
  EXPECT_EQ(std::memcmp(buffer, expected_.data() + offset, length), 0);
  EXPECT_EQ(iree_hal_file_requirements(file_), requirements);
  iree_hal_file_release(file_);
  file_ = nullptr;

  iree_io_file_contents_t* contents = nullptr;
  IREE_ASSERT_OK(iree_io_file_contents_read(
      path_.path_view(), iree_allocator_system(), &contents));
  EXPECT_EQ(contents->const_buffer.data_length, expected_.size());
  if (contents->const_buffer.data_length == expected_.size()) {
    EXPECT_EQ(std::memcmp(contents->const_buffer.data, expected_.data(),
                          expected_.size()),
              0);
  }
  iree_io_file_contents_free(contents);
}

INSTANTIATE_TEST_SUITE_P(
    NativeModes, FdFileTest,
    ::testing::Values(iree_io_file_mode_t{0},
                      iree_io_file_mode_t{IREE_IO_FILE_MODE_ASYNC},
                      iree_io_file_mode_t{IREE_IO_FILE_MODE_DIRECT},
                      iree_io_file_mode_t{IREE_IO_FILE_MODE_DIRECT |
                                          IREE_IO_FILE_MODE_ASYNC}));

}  // namespace

#endif  // IREE_FILE_IO_ENABLE
