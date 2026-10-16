// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "libamdf/src/xdna/umd/firmware_trace.h"

#include <errno.h>
#include <sys/ioctl.h>

#include <array>
#include <cstdarg>
#include <cstring>

#include "gtest/gtest.h"
#include "libamdf/src/xdna/umd/drm/device.h"

namespace {

struct NativeTraceState {
  // Native error; ESTALE also publishes restart metadata.
  int error = 0;
  // Returned native configuration.
  std::array<uint32_t, 4> configuration = {0x200, 1, 5, 0};
  // End of the native read, not the producer limit.
  uint64_t position = 128;
  // Bytes copied into the destination.
  uint32_t size = 32;
  // Number of native requests, including failures.
  uint32_t calls = 0;
};

NativeTraceState* state;

}  // namespace

extern "C" int __wrap_ioctl(int descriptor, unsigned long operation, ...) {
  EXPECT_EQ(descriptor, 99);
  ++state->calls;
  va_list arguments;
  va_start(arguments, operation);
  auto* request = va_arg(arguments, uint32_t*);
  va_end(arguments);
  if (state->error != 0 && state->error != ESTALE) {
    errno = state->error;
    return -1;
  }
  if (_IOC_NR(operation) == 0x48) {
    EXPECT_EQ(request[0], 6u);
    EXPECT_EQ(request[1], 16u);
    uint64_t address;
    std::memcpy(&address, request + 2, 8);
    const auto* configuration = reinterpret_cast<const uint32_t*>(address);
    EXPECT_EQ(configuration[0], 1u);
    EXPECT_EQ(configuration[1], 5u);
  } else {
    if (_IOC_NR(operation) != 0x4a) {
      ADD_FAILURE() << "unexpected ioctl " << operation;
      errno = EINVAL;
      return -1;
    }
    EXPECT_EQ(_IOC_NR(operation), 0x4au);
    EXPECT_EQ(_IOC_SIZE(operation), 24u);
    EXPECT_EQ(request[2], 1u);
    uint64_t address;
    std::memcpy(&address, request + 4, 8);
    auto* buffer = reinterpret_cast<uint8_t*>(address);
    if (request[0] == 8) {
      EXPECT_EQ(request[1], 16u);
      std::memcpy(buffer, state->configuration.data(), 16);
    } else {
      EXPECT_EQ(request[0], 4u);
      auto* metadata = buffer + request[1] - 16;
      EXPECT_EQ(metadata[12], 0u);
      std::memcpy(metadata, &state->position, 8);
      std::memcpy(metadata + 8, &state->size, 4);
    }
  }
  errno = state->error;
  return state->error == 0 ? 0 : -1;
}

namespace {

class LinuxXdnaFirmwareTraceTest : public ::testing::Test {
 protected:
  void SetUp() override {
    state = &state_;
    device_.descriptor = 99;
  }
  void TearDown() override { state = nullptr; }
  // External ioctl replies for this test's provider connection.
  NativeTraceState state_;
  // Device has no workload context or private result buffer.
  amdf_xdna_umd_device_t device_ = {};
};

TEST_F(LinuxXdnaFirmwareTraceTest, QueriesAndConfiguresSdkService) {
  amdf_xdna_firmware_trace_info_t info = {};
  ASSERT_EQ(amdf_xdna_umd_query_firmware_trace_info(&device_, &info),
            AMDF_STATUS_OK);
  EXPECT_EQ(info.enabled, 1u);
  EXPECT_EQ(info.format_version, 0x200u);
  EXPECT_EQ(info.categories, 5u);
  EXPECT_EQ(amdf_xdna_umd_configure_firmware_trace(&device_, 1, 5),
            AMDF_STATUS_OK);
}

TEST_F(LinuxXdnaFirmwareTraceTest, CapacityLimitedCopyStillHasAnExactCursor) {
  std::array<uint64_t, 6> buffer = {};
  amdf_observation_read_result_t result = {};
  ASSERT_EQ(amdf_xdna_umd_read_firmware_trace(&device_, 64, buffer.data(),
                                              sizeof(buffer), &result),
            AMDF_STATUS_OK);
  EXPECT_EQ(result.next_position, 128u);
  EXPECT_EQ(result.byte_length, 32u);
  EXPECT_EQ(result.lost_byte_length, 32u);
  EXPECT_EQ(result.flags, AMDF_OBSERVATION_READ_FLAG_GAP);
}

TEST_F(LinuxXdnaFirmwareTraceTest, StaleCursorPublishesRestartMetadata) {
  state_.error = ESTALE;
  state_.position = 0;
  state_.size = 0;
  std::array<uint64_t, 6> buffer = {};
  amdf_observation_read_result_t result = {};
  ASSERT_EQ(amdf_xdna_umd_read_firmware_trace(&device_, 1024, buffer.data(),
                                              sizeof(buffer), &result),
            AMDF_STATUS_OK);
  EXPECT_EQ(result.next_position, 0u);
  EXPECT_EQ(result.byte_length, 0u);
  EXPECT_EQ(result.lost_byte_length, UINT64_MAX);
  EXPECT_EQ(result.flags, AMDF_OBSERVATION_READ_FLAG_GAP |
                              AMDF_OBSERVATION_READ_FLAG_RESTART);
}

TEST_F(LinuxXdnaFirmwareTraceTest, MissingQueryLeavesStateUnpublished) {
  for (int error : {EINVAL, EOPNOTSUPP, ENOTTY, EPERM, EIO, ESHUTDOWN}) {
    SCOPED_TRACE(error);
    state_.error = error;
    amdf_xdna_firmware_trace_info_t info;
    std::memset(&info, 0xA5, sizeof(info));
    const auto original = info;
    const amdf_status_t expected =
        error == EINVAL || error == EOPNOTSUPP || error == ENOTTY
            ? amdf_make_api_status(AMDF_STATUS_CODE_UNSUPPORTED)
            : amdf_make_status(AMDF_STATUS_DOMAIN_ERRNO, error);
    EXPECT_EQ(amdf_xdna_umd_query_firmware_trace_info(&device_, &info),
              expected);
    EXPECT_EQ(std::memcmp(&info, &original, sizeof(info)), 0);
  }
}

TEST_F(LinuxXdnaFirmwareTraceTest, DistinguishesAbsenceAndControlFailure) {
  for (int error : {EINVAL, EOPNOTSUPP, ENOTTY, EPERM, ESHUTDOWN}) {
    SCOPED_TRACE(error);
    state_.error = error;
    state_.calls = 0;
    std::array<uint64_t, 6> buffer = {};
    amdf_observation_read_result_t result = {.next_position = UINT64_MAX};
    const auto original = result;
    const amdf_status_t expected =
        error == EOPNOTSUPP || error == ENOTTY
            ? amdf_make_api_status(AMDF_STATUS_CODE_UNSUPPORTED)
            : amdf_make_status(AMDF_STATUS_DOMAIN_ERRNO, error);
    EXPECT_EQ(amdf_xdna_umd_read_firmware_trace(&device_, 0, buffer.data(),
                                                sizeof(buffer), &result),
              expected);
    EXPECT_EQ(std::memcmp(&result, &original, sizeof(result)), 0);
    EXPECT_EQ(state_.calls, 1u);
    EXPECT_EQ(amdf_xdna_umd_configure_firmware_trace(&device_, 1, 5), expected);
  }
}

}  // namespace
