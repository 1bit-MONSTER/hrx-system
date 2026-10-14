// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "libamdf/src/xdna/umd/clock.h"

#include <drm/amdxdna_accel.h>

#include <cerrno>
#include <cstdarg>
#include <cstring>

#include "gtest/gtest.h"
#include "libamdf/src/xdna/umd/drm/device.h"

namespace {
struct NativeClockState {
  // Array clock from the native telemetry/configuration snapshot, in MHz.
  uint32_t frequency_mhz = 1800;
  // Error injected at the ioctl boundary before output publication.
  int error = 0;
};
thread_local NativeClockState* native_state = nullptr;
}  // namespace

extern "C" int __real_ioctl(int descriptor, unsigned long request, ...);
extern "C" int __wrap_ioctl(int descriptor, unsigned long request, ...) {
  va_list arguments;
  va_start(arguments, request);
  auto* argument = va_arg(arguments, void*);
  va_end(arguments);
  if (native_state == nullptr) {
    return __real_ioctl(descriptor, request, argument);
  }
  EXPECT_EQ(descriptor, 23);
  EXPECT_EQ(request, DRM_IOCTL_AMDXDNA_GET_INFO);
  auto* query = static_cast<amdxdna_drm_get_info*>(argument);
  EXPECT_EQ(query->param, DRM_AMDXDNA_QUERY_CLOCK_METADATA);
  EXPECT_EQ(query->buffer_size, sizeof(amdxdna_drm_query_clock_metadata));
  if (native_state->error != 0) {
    errno = native_state->error;
    return -1;
  }
  auto* clocks =
      reinterpret_cast<amdxdna_drm_query_clock_metadata*>(query->buffer);
  clocks->h_clock.freq_mhz = native_state->frequency_mhz;
  clocks->mp_npu_clock.freq_mhz = 1267;
  return 0;
}

namespace {
class LinuxXdnaClockTest : public ::testing::Test {
 protected:
  void SetUp() override {
    native_state = &native_;
    device_.descriptor = 23;
  }
  void TearDown() override { native_state = nullptr; }
  // Case-owned reply at the native driver boundary.
  NativeClockState native_;
  // Existing connection; the query creates no device or context.
  amdf_xdna_umd_device_t device_ = {};
};

TEST_F(LinuxXdnaClockTest, SamplesArrayRateWithoutCachingOrControllerAlias) {
  for (uint32_t rate : {800u, 1800u, UINT32_MAX}) {
    native_.frequency_mhz = rate;
    amdf_observation_clock_info_t info = {};
    ASSERT_EQ(amdf_xdna_umd_query_timer_clock_info(&device_, &info),
              AMDF_STATUS_OK);
    EXPECT_EQ(info.frequency_hz, uint64_t{rate} * 1000000);
    EXPECT_EQ(info.counter_bit_count, 64u);
    EXPECT_EQ(info.reserved, 0u);
  }
}

TEST_F(LinuxXdnaClockTest, PreservesOutputsOnAbsentRateAndNativeFailure) {
  const amdf_observation_clock_info_t sentinel = {1234, 56, 99};
  amdf_observation_clock_info_t info = sentinel;
  native_.frequency_mhz = 0;
  EXPECT_EQ(amdf_xdna_umd_query_timer_clock_info(&device_, &info),
            amdf_make_api_status(AMDF_STATUS_CODE_UNSUPPORTED));
  EXPECT_EQ(std::memcmp(&info, &sentinel, sizeof(info)), 0);
  for (int error : {EOPNOTSUPP, ENOTTY, EPERM, EIO}) {
    native_.error = error;
    EXPECT_EQ(amdf_xdna_umd_query_timer_clock_info(&device_, &info),
              error == EOPNOTSUPP || error == ENOTTY
                  ? amdf_make_api_status(AMDF_STATUS_CODE_UNSUPPORTED)
                  : amdf_make_status(AMDF_STATUS_DOMAIN_ERRNO, error));
    EXPECT_EQ(std::memcmp(&info, &sentinel, sizeof(info)), 0);
  }
}
}  // namespace
