// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "libamdf/src/gpu/umd/clock.h"

#include <drm/amdgpu_drm.h>

#include <cerrno>
#include <cstdarg>
#include <cstring>

#include "gtest/gtest.h"
#include "libamdf/src/gpu/umd/kfd/device.h"

namespace {

struct NativeClockState {
  // Native counter frequency in kHz, independent of shader operating clocks.
  uint32_t frequency_khz = 99810;
  // Native ioctl error injected before output publication.
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
  EXPECT_EQ(request, DRM_IOCTL_AMDGPU_INFO);
  auto* query = static_cast<drm_amdgpu_info*>(argument);
  EXPECT_EQ(query->query, AMDGPU_INFO_DEV_INFO);
  EXPECT_EQ(query->return_size, sizeof(drm_amdgpu_info_device));
  if (native_state->error != 0) {
    errno = native_state->error;
    return -1;
  }
  auto* info = reinterpret_cast<drm_amdgpu_info_device*>(query->return_pointer);
  info->gpu_counter_freq = native_state->frequency_khz;
  info->max_engine_clock = 3100000;
  return 0;
}

namespace {

class KfdClockTest : public ::testing::Test {
 protected:
  void SetUp() override {
    native_state = &native_;
    device_.render_descriptor = 23;
  }
  void TearDown() override { native_state = nullptr; }
  // Case-owned response at the native driver boundary.
  NativeClockState native_;
  // Borrowed render connection; no device construction is mocked.
  amdf_gpu_umd_device_t device_ = {};
};

TEST_F(KfdClockTest, ConvertsNativeCounterKilohertzWithoutOperatingClock) {
  for (uint32_t rate : {99810u, 100000u, UINT32_MAX}) {
    native_.frequency_khz = rate;
    for (auto type :
         {AMDF_QUEUE_COMMAND_TYPE_GPU_PM4, AMDF_QUEUE_COMMAND_TYPE_GPU_AQL,
          AMDF_QUEUE_COMMAND_TYPE_GPU_SDMA}) {
      amdf_observation_clock_info_t info = {};
      ASSERT_EQ(amdf_gpu_umd_query_queue_clock_info(&device_, type, &info),
                AMDF_STATUS_OK);
      EXPECT_EQ(info.frequency_hz, uint64_t{rate} * 1000);
      EXPECT_EQ(info.counter_bit_count, 0u);
      EXPECT_EQ(info.reserved, 0u);
    }
  }
}

TEST_F(KfdClockTest, PreservesOutputOnMissingRateAndNativeFailure) {
  const amdf_observation_clock_info_t sentinel = {1234, 56, 99};
  amdf_observation_clock_info_t info = sentinel;
  native_.frequency_khz = 0;
  EXPECT_EQ(amdf_gpu_umd_query_queue_clock_info(
                &device_, AMDF_QUEUE_COMMAND_TYPE_GPU_SDMA, &info),
            amdf_make_api_status(AMDF_STATUS_CODE_UNSUPPORTED));
  EXPECT_EQ(std::memcmp(&info, &sentinel, sizeof(info)), 0);
  native_.error = EIO;
  EXPECT_EQ(amdf_gpu_umd_query_queue_clock_info(
                &device_, AMDF_QUEUE_COMMAND_TYPE_GPU_PM4, &info),
            amdf_make_status(AMDF_STATUS_DOMAIN_ERRNO, EIO));
  EXPECT_EQ(std::memcmp(&info, &sentinel, sizeof(info)), 0);
}

}  // namespace
