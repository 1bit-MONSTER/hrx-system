// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "libamdf/src/xdna/umd/clock.h"

#include <cstring>

#include "gtest/gtest.h"
#include "libamdf/src/xdna/umd/mcdm/device.h"

namespace {
struct NativeClockState {
  // Native operating-clock list; only its first entry is the array rate.
  const char* text = "1810\n400\n";
  // Serialized output extent, including native payload metadata.
  uint64_t extent = 4112;
  // Failure injected before writing the native response.
  NTSTATUS status = 0;
};
NativeClockState* native_state;

NTSTATUS APIENTRY Escape(const D3DKMT_ESCAPE* request) {
  EXPECT_EQ(request->hAdapter, 1u);
  EXPECT_EQ(request->hDevice, 2u);
  EXPECT_EQ(request->hContext, 0u);
  EXPECT_EQ(request->Type, D3DKMT_ESCAPE_DRIVERPRIVATE);
  EXPECT_EQ(request->PrivateDriverDataSize, 4216u);
  auto* packet = static_cast<uint32_t*>(request->pPrivateDriverData);
  EXPECT_EQ(packet[0], 4u);
  EXPECT_EQ(packet[1], 4200u);
  EXPECT_EQ(packet[4], 8u);
  EXPECT_EQ(packet[8], 48u);
  EXPECT_EQ(packet[12], 1u);
  EXPECT_EQ(packet[13], 4096u);
  EXPECT_EQ(packet[16], 4112u);
  EXPECT_EQ(packet[22], 1u);
  if (native_state->status != 0) {
    return native_state->status;
  }
  std::memcpy(packet + 24, &native_state->extent, 8);
  if (native_state->text != nullptr) {
    std::memcpy(packet + 26, native_state->text,
                std::strlen(native_state->text) + 1);
  } else {
    std::memset(packet + 26, '1', 4096);
  }
  return 0;
}

class WindowsXdnaClockTest : public ::testing::Test {
 protected:
  void SetUp() override {
    native_state = &native_;
    kmt_.escape = Escape;
    device_.kmt = &kmt_;
    device_.adapter = 1;
    device_.device = 2;
  }
  void TearDown() override { native_state = nullptr; }
  // Case-owned native reply; the provider remains real.
  NativeClockState native_;
  // Only the syscall dependency is replaced.
  amdf_kmt_api_t kmt_ = {};
  // Existing logical device; the query creates no context.
  amdf_xdna_umd_device_t device_ = {};
};

TEST_F(WindowsXdnaClockTest, SamplesFirstClockWithoutCachingOrOtherClockAlias) {
  for (auto text : {"800\n400\n", "1810\n1\n", "4294967295\n5000\n"}) {
    native_.text = text;
    amdf_observation_clock_info_t info = {};
    ASSERT_EQ(amdf_xdna_umd_query_timer_clock_info(&device_, &info),
              AMDF_STATUS_OK);
    const uint64_t expected = text[0] == '8'   ? 800
                              : text[0] == '1' ? 1810
                                               : UINT32_MAX;
    EXPECT_EQ(info.frequency_hz, expected * 1000000);
    EXPECT_EQ(info.counter_bit_count, 64u);
    EXPECT_EQ(info.reserved, 0u);
  }
}

TEST_F(WindowsXdnaClockTest, RejectsMalformedExternalReplyWithoutOutput) {
  const amdf_observation_clock_info_t sentinel = {1234, 56, 99};
  amdf_observation_clock_info_t info = sentinel;
  for (const char* text : {"", "-800\n", "800MHz\n", "800", "4294967296\n",
                           static_cast<const char*>(nullptr)}) {
    native_.text = text;
    EXPECT_EQ(amdf_xdna_umd_query_timer_clock_info(&device_, &info),
              amdf_make_api_status(AMDF_STATUS_CODE_INTERNAL));
    EXPECT_EQ(std::memcmp(&info, &sentinel, sizeof(info)), 0);
  }
  native_.text = "800\n400\n";
  native_.extent = UINT64_MAX;
  EXPECT_EQ(amdf_xdna_umd_query_timer_clock_info(&device_, &info),
            amdf_make_api_status(AMDF_STATUS_CODE_INTERNAL));
  EXPECT_EQ(std::memcmp(&info, &sentinel, sizeof(info)), 0);
}

TEST_F(WindowsXdnaClockTest, DistinguishesMissingRateServiceAndPermission) {
  const amdf_observation_clock_info_t sentinel = {1234, 56, 99};
  amdf_observation_clock_info_t info = sentinel;
  native_.text = "0\n400\n";
  EXPECT_EQ(amdf_xdna_umd_query_timer_clock_info(&device_, &info),
            amdf_make_api_status(AMDF_STATUS_CODE_UNSUPPORTED));
  EXPECT_EQ(std::memcmp(&info, &sentinel, sizeof(info)), 0);
  for (uint32_t code : {0xc00000bbu, 0xc0000022u}) {
    native_.status = static_cast<NTSTATUS>(code);
    EXPECT_EQ(amdf_xdna_umd_query_timer_clock_info(&device_, &info),
              code == 0xc00000bb
                  ? amdf_make_api_status(AMDF_STATUS_CODE_UNSUPPORTED)
                  : amdf_kmt_make_status(native_.status));
    EXPECT_EQ(std::memcmp(&info, &sentinel, sizeof(info)), 0);
  }
  kmt_.escape = nullptr;
  EXPECT_EQ(amdf_xdna_umd_query_timer_clock_info(&device_, &info),
            amdf_make_api_status(AMDF_STATUS_CODE_UNSUPPORTED));
  EXPECT_EQ(std::memcmp(&info, &sentinel, sizeof(info)), 0);
}
}  // namespace
