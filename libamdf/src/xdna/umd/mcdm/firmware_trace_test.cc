// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "libamdf/src/xdna/umd/firmware_trace.h"

#include <array>
#include <cstring>

#include "gtest/gtest.h"
#include "libamdf/src/xdna/umd/mcdm/context.h"
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
  // Number of native RTOS task entries in the reply.
  uint64_t task_count = 16;
  // Driver selectors indexed by physical firmware slot, including unused tail.
  std::array<uint64_t, 33> selectors;
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
  if (packet[0] == 11) {
    EXPECT_EQ(request->PrivateDriverDataSize, 8296u);
    // Serialized header of the native telemetry query used by XRT's
    // preemption report; output begins at byte 88.
    constexpr uint32_t header[] = {11, 8280, 0, 0,    0, 0, 1, 0, 40, 0, 0,
                                   0,  0,    0, 8200, 0, 1, 0, 0, 0,  1, 0};
    EXPECT_EQ(std::memcmp(packet, header, sizeof(header)), 0);
    auto* bytes = reinterpret_cast<uint8_t*>(packet);
    std::memcpy(bytes + 96, &state->task_count, sizeof(state->task_count));
    for (uint32_t i = 0; i < state->selectors.size(); ++i) {
      std::memcpy(bytes + 312 + i * 248, &state->selectors[i], 8);
    }
  } else if (packet[0] == 21) {
    EXPECT_EQ(request->PrivateDriverDataSize, 112u);
    EXPECT_EQ(packet[4], 4u);
    EXPECT_EQ(packet[8], 44u);
    EXPECT_EQ(packet[15], 12u);
    if (packet[12]) {
      state->enabled = packet[23] != 0;
    } else {
      // Padding is not part of the native enabled byte.
      packet[23] = 0xa5a5a500u | static_cast<uint32_t>(state->enabled);
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
    context_.device = &device_;
    context_.command_aperture_cookie = 37;
    state_.selectors.fill(UINT64_MAX);
  }
  void TearDown() override { state = nullptr; }
  // Injected native syscall response.
  NativeTraceState state_;
  // Only the provider dependency under test is substituted.
  amdf_kmt_api_t kmt_ = {};
  // Ordinary logical device with no workload context or queue.
  amdf_xdna_umd_device_t device_ = {};
  // Native context identity borrowed from the ordinary context owner.
  amdf_xdna_umd_context_t context_ = {};
};

TEST_F(WindowsXdnaFirmwareTraceTest, SamplesCurrentOwnerWithoutCaching) {
  state_.selectors[14] = 81;
  amdf_xdna_context_firmware_trace_info_t info = {};
  for (uint32_t id : {15u, 2u, UINT32_MAX, 0u}) {
    state_.selectors[15] = UINT64_MAX;
    state_.selectors[2] = UINT64_MAX;
    state_.selectors[0] = UINT64_MAX;
    if (id != UINT32_MAX) {
      state_.selectors[id] = 37;
    }
    ASSERT_EQ(amdf_xdna_umd_context_query_firmware_trace_info(&context_, &info),
              AMDF_STATUS_OK);
    EXPECT_EQ(info.firmware_context_id, id);
    EXPECT_EQ(info.reserved, 0u);
  }
  EXPECT_EQ(state_.calls, 4u);
}

TEST_F(WindowsXdnaFirmwareTraceTest, ZeroSelectorIsAValidNativeOwner) {
  context_.command_aperture_cookie = 0;
  state_.selectors[7] = 0;
  amdf_xdna_context_firmware_trace_info_t info = {};
  ASSERT_EQ(amdf_xdna_umd_context_query_firmware_trace_info(&context_, &info),
            AMDF_STATUS_OK);
  EXPECT_EQ(info.firmware_context_id, 7u);
}

TEST_F(WindowsXdnaFirmwareTraceTest, ReadsOnlyTheReturnedTaskExtent) {
  state_.task_count = 4;
  state_.selectors[4] = 37;
  amdf_xdna_context_firmware_trace_info_t info = {};
  ASSERT_EQ(amdf_xdna_umd_context_query_firmware_trace_info(&context_, &info),
            AMDF_STATUS_OK);
  EXPECT_EQ(info.firmware_context_id, AMDF_XDNA_FIRMWARE_CONTEXT_ID_UNASSIGNED);
  state_.task_count = state_.selectors.size();
  state_.selectors[4] = UINT64_MAX;
  state_.selectors.back() = 37;
  ASSERT_EQ(amdf_xdna_umd_context_query_firmware_trace_info(&context_, &info),
            AMDF_STATUS_OK);
  EXPECT_EQ(info.firmware_context_id, state_.selectors.size() - 1);
}

TEST_F(WindowsXdnaFirmwareTraceTest, RejectsOversizedAndAmbiguousNativeMaps) {
  for (bool oversized : {false, true}) {
    SCOPED_TRACE(oversized);
    state_.task_count = oversized ? UINT64_MAX : 16;
    state_.selectors[4] = 37;
    state_.selectors[5] = 37;
    amdf_xdna_context_firmware_trace_info_t info = {.firmware_context_id = 55};
    const auto original = info;
    EXPECT_EQ(amdf_xdna_umd_context_query_firmware_trace_info(&context_, &info),
              amdf_make_api_status(AMDF_STATUS_CODE_INTERNAL));
    EXPECT_EQ(std::memcmp(&info, &original, sizeof(info)), 0);
  }
}

TEST_F(WindowsXdnaFirmwareTraceTest, PreservesContextQueryAbsenceAndErrors) {
  for (uint32_t native_status : {0xc00000bbu, 0xc0000022u, 0xc0000001u}) {
    SCOPED_TRACE(native_status);
    state_.status = static_cast<NTSTATUS>(native_status);
    state_.calls = 0;
    amdf_xdna_context_firmware_trace_info_t info = {.firmware_context_id = 55};
    const auto original = info;
    const amdf_status_t expected =
        native_status == 0xc00000bbu
            ? amdf_make_api_status(AMDF_STATUS_CODE_UNSUPPORTED)
            : amdf_kmt_make_status(state_.status);
    EXPECT_EQ(amdf_xdna_umd_context_query_firmware_trace_info(&context_, &info),
              expected);
    EXPECT_EQ(std::memcmp(&info, &original, sizeof(info)), 0);
    EXPECT_EQ(state_.calls, 1u);
  }
}

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
