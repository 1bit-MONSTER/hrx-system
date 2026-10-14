// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "libamdf/src/xdna/umd/firmware_trace.h"

#include <array>
#include <cstring>

#include "gtest/gtest.h"
#include "libamdf/src/xdna/umd/mcdm/device.h"

namespace {

struct NativeTraceState {
  // Native status returned before touching outputs.
  NTSTATUS status = 0;
  // Bytes copied from the native ring into a read destination.
  uint64_t size = 0;
  // Native observed producer limit, independent of copy capacity.
  uint64_t limit = 0;
  // Capture state for configuration-query replies.
  bool enabled = false;
  // Number of native calls, including failed calls.
  uint32_t calls = 0;
};

NativeTraceState* state;

NTSTATUS APIENTRY Escape(const D3DKMT_ESCAPE* request) {
  ++state->calls;
  EXPECT_EQ(request->hAdapter, 1u);
  EXPECT_EQ(request->hDevice, 2u);
  EXPECT_EQ(request->hContext, 0u);
  EXPECT_EQ(request->Type, D3DKMT_ESCAPE_DRIVERPRIVATE);
  auto* packet = static_cast<uint32_t*>(request->pPrivateDriverData);
  if (state->status != 0) {
    return state->status;
  }
  if (packet[0] == 21) {
    EXPECT_EQ(request->PrivateDriverDataSize, 112u);
    EXPECT_EQ(packet[4], 4u);
    EXPECT_EQ(packet[8], 44u);
    EXPECT_EQ(packet[15], 12u);
    if (packet[12]) {
      state->enabled = packet[23] != 0;
    } else {
      // Padding is not part of the native enabled byte.
      packet[23] = 0xa5a5a500 | state->enabled;
      packet[24] = 5;
      packet[25] = 0x200;
    }
  } else {
    EXPECT_EQ(packet[0], 22u);
    EXPECT_EQ(packet[8], 40u);
    EXPECT_EQ(packet[14] + 96, request->PrivateDriverDataSize);
    std::memcpy(packet + 22, &state->size, 8);
    std::memcpy(packet + 24, &state->limit, 8);
  }
  return 0;
}

class WindowsXdnaFirmwareTraceTest : public ::testing::Test {
 protected:
  void SetUp() override {
    state = &state_;
    kmt_.escape = Escape;
    device_.kmt = &kmt_;
    device_.adapter = 1;
    device_.device = 2;
  }
  void TearDown() override { state = nullptr; }
  // Injected native syscall response.
  NativeTraceState state_;
  // Only the provider dependency under test is substituted.
  amdf_kmt_api_t kmt_ = {};
  // Ordinary logical device with no workload context or queue.
  amdf_xdna_umd_device_t device_ = {};
};

TEST_F(WindowsXdnaFirmwareTraceTest, DisabledVersionIsNotAFormatOrAbsence) {
  amdf_xdna_firmware_trace_info_t info = {};
  ASSERT_EQ(amdf_xdna_umd_query_firmware_trace_info(&device_, &info),
            AMDF_STATUS_OK);
  EXPECT_EQ(info.enabled, 0u);
  EXPECT_EQ(info.format_version, 0u);
  ASSERT_EQ(amdf_xdna_umd_configure_firmware_trace(&device_, 1, 5),
            AMDF_STATUS_OK);
  ASSERT_EQ(amdf_xdna_umd_query_firmware_trace_info(&device_, &info),
            AMDF_STATUS_OK);
  EXPECT_EQ(info.enabled, 1u);
  EXPECT_EQ(info.categories, 5u);
  EXPECT_EQ(info.format_version, 0x200u);
}

TEST_F(WindowsXdnaFirmwareTraceTest,
       TruncatedCopyDoesNotAdvanceCallerProgress) {
  std::array<uint64_t, (256 + 112) / 8> buffer;
  state_.size = 256;
  state_.limit = 1376;
  amdf_observation_read_result_t result = {.next_position = UINT64_MAX};
  const auto original = result;
  EXPECT_EQ(amdf_xdna_umd_read_firmware_trace(&device_, 1056, buffer.data(),
                                              sizeof(buffer), &result),
            amdf_make_api_status(AMDF_STATUS_CODE_BUFFER_TOO_SMALL));
  EXPECT_EQ(std::memcmp(&result, &original, sizeof(result)), 0);
  EXPECT_EQ(state_.calls, 1u);
}

TEST_F(WindowsXdnaFirmwareTraceTest, CompleteRetainedWindowReportsPrefixLoss) {
  std::array<uint64_t, (512 + 112) / 8> buffer;
  state_.size = 256;
  state_.limit = 1376;
  amdf_observation_read_result_t result = {};
  ASSERT_EQ(amdf_xdna_umd_read_firmware_trace(&device_, 32, buffer.data(),
                                              sizeof(buffer), &result),
            AMDF_STATUS_OK);
  EXPECT_EQ(result.next_position, 1376u);
  EXPECT_EQ(result.byte_offset, 104u);
  EXPECT_EQ(result.byte_length, 256u);
  EXPECT_EQ(result.lost_byte_length, 1088u);
  EXPECT_EQ(result.flags, AMDF_OBSERVATION_READ_FLAG_GAP);
}

TEST_F(WindowsXdnaFirmwareTraceTest, RestartDoesNotUnderflowLossAccounting) {
  std::array<uint64_t, (512 + 112) / 8> buffer;
  state_.size = 32;
  state_.limit = 32;
  amdf_observation_read_result_t result = {};
  ASSERT_EQ(amdf_xdna_umd_read_firmware_trace(&device_, 1024, buffer.data(),
                                              sizeof(buffer), &result),
            AMDF_STATUS_OK);
  EXPECT_EQ(result.next_position, 32u);
  EXPECT_EQ(result.byte_length, 0u);
  EXPECT_EQ(result.lost_byte_length, UINT64_MAX);
  EXPECT_EQ(result.flags, AMDF_OBSERVATION_READ_FLAG_GAP |
                              AMDF_OBSERVATION_READ_FLAG_RESTART);
}

TEST_F(WindowsXdnaFirmwareTraceTest, PreservesPermissionFailureWithoutRetry) {
  state_.status = static_cast<NTSTATUS>(0xc0000022u);
  amdf_xdna_firmware_trace_info_t info = {.enabled = UINT32_MAX};
  const auto original = info;
  EXPECT_EQ(amdf_xdna_umd_query_firmware_trace_info(&device_, &info),
            amdf_kmt_make_status(state_.status));
  EXPECT_EQ(std::memcmp(&info, &original, sizeof(info)), 0);
  EXPECT_EQ(state_.calls, 1u);
}

}  // namespace
