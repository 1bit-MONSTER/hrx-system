// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/target/arch/amdgpu/abi/asan.h"

#include <cstddef>
#include <cstdint>

#include "iree/hal/drivers/amdgpu/abi/asan.h"
#include "iree/testing/gtest.h"

namespace {

TEST(AmdgpuAsanAbiTest, ConstantsMatchHalAsanAbi) {
  EXPECT_STREQ(LOOM_AMDGPU_ASAN_CONFIG_GLOBAL_NAME,
               IREE_HAL_AMDGPU_ASAN_CONFIG_GLOBAL_NAME);
  EXPECT_EQ(LOOM_AMDGPU_ASAN_CONFIG_ABI_VERSION,
            IREE_HAL_AMDGPU_ASAN_CONFIG_ABI_VERSION_0);
  EXPECT_EQ(LOOM_AMDGPU_ASAN_CONFIG_BYTE_LENGTH,
            sizeof(iree_hal_amdgpu_asan_config_t));
  EXPECT_EQ(LOOM_AMDGPU_ASAN_CONFIG_RECORD_LENGTH_OFFSET,
            offsetof(iree_hal_amdgpu_asan_config_t, record_length));
  EXPECT_EQ(LOOM_AMDGPU_ASAN_CONFIG_ABI_VERSION_OFFSET,
            offsetof(iree_hal_amdgpu_asan_config_t, abi_version));
  EXPECT_EQ(LOOM_AMDGPU_ASAN_CONFIG_FLAGS_OFFSET,
            offsetof(iree_hal_amdgpu_asan_config_t, flags));
  EXPECT_EQ(LOOM_AMDGPU_ASAN_CONFIG_SHADOW_SCALE_SHIFT_OFFSET,
            offsetof(iree_hal_amdgpu_asan_config_t, shadow_scale_shift));
  EXPECT_EQ(LOOM_AMDGPU_ASAN_CONFIG_SHADOW_BASE_OFFSET,
            offsetof(iree_hal_amdgpu_asan_config_t, shadow_base));
  EXPECT_EQ(LOOM_AMDGPU_ASAN_CONFIG_APPLICATION_WINDOW_BASE_OFFSET,
            offsetof(iree_hal_amdgpu_asan_config_t, application_window_base));
  EXPECT_EQ(LOOM_AMDGPU_ASAN_CONFIG_APPLICATION_WINDOW_SIZE_OFFSET,
            offsetof(iree_hal_amdgpu_asan_config_t, application_window_size));
  EXPECT_EQ(LOOM_AMDGPU_ASAN_CONFIG_SHADOW_SIZE_OFFSET,
            offsetof(iree_hal_amdgpu_asan_config_t, shadow_size));
  EXPECT_EQ(LOOM_AMDGPU_ASAN_CONFIG_SHADOW_SLAB_SIZE_OFFSET,
            offsetof(iree_hal_amdgpu_asan_config_t, shadow_slab_size));
  EXPECT_EQ(LOOM_AMDGPU_ASAN_CONFIG_RESERVED_ARRAY_0_OFFSET,
            offsetof(iree_hal_amdgpu_asan_config_t, reserved[0]));
  EXPECT_EQ(LOOM_AMDGPU_ASAN_CONFIG_RESERVED_ARRAY_1_OFFSET,
            offsetof(iree_hal_amdgpu_asan_config_t, reserved[1]));
  EXPECT_EQ(LOOM_AMDGPU_ASAN_CONFIG_RESERVED_ARRAY_2_OFFSET,
            offsetof(iree_hal_amdgpu_asan_config_t, reserved[2]));
  EXPECT_EQ(LOOM_AMDGPU_ASAN_CONFIG_RESERVED_ARRAY_3_OFFSET,
            offsetof(iree_hal_amdgpu_asan_config_t, reserved[3]));
  EXPECT_EQ(LOOM_AMDGPU_ASAN_CONFIG_RESERVED_ARRAY_4_OFFSET,
            offsetof(iree_hal_amdgpu_asan_config_t, reserved[4]));
  EXPECT_EQ(static_cast<uint32_t>(LOOM_AMDGPU_ASAN_CONFIG_FLAG_NONE),
            static_cast<uint32_t>(IREE_HAL_AMDGPU_ASAN_CONFIG_FLAG_NONE));
  EXPECT_EQ(static_cast<uint32_t>(LOOM_AMDGPU_ASAN_CONFIG_FLAG_ENABLED),
            static_cast<uint32_t>(IREE_HAL_AMDGPU_ASAN_CONFIG_FLAG_ENABLED));
}

TEST(AmdgpuAsanAbiTest, UsesStandardShadowScaleForAccessChecks) {
  EXPECT_EQ(LOOM_AMDGPU_ASAN_SHADOW_SCALE_SHIFT, 3u);
}

TEST(AmdgpuAsanAbiTest, ReportConstantsMatchHalAsanAbi) {
  EXPECT_EQ(LOOM_AMDGPU_ASAN_REPORT_ABI_VERSION,
            IREE_HAL_AMDGPU_ASAN_REPORT_ABI_VERSION_0);
  EXPECT_EQ(LOOM_AMDGPU_ASAN_REPORT_BYTE_LENGTH,
            sizeof(iree_hal_amdgpu_asan_report_t));
  EXPECT_EQ(LOOM_AMDGPU_ASAN_REPORT_RECORD_LENGTH_OFFSET,
            offsetof(iree_hal_amdgpu_asan_report_t, record_length));
  EXPECT_EQ(LOOM_AMDGPU_ASAN_REPORT_ABI_VERSION_OFFSET,
            offsetof(iree_hal_amdgpu_asan_report_t, abi_version));
  EXPECT_EQ(LOOM_AMDGPU_ASAN_REPORT_ACCESS_KIND_OFFSET,
            offsetof(iree_hal_amdgpu_asan_report_t, access_kind));
  EXPECT_EQ(LOOM_AMDGPU_ASAN_REPORT_FLAGS_OFFSET,
            offsetof(iree_hal_amdgpu_asan_report_t, flags));
  EXPECT_EQ(LOOM_AMDGPU_ASAN_REPORT_FAULT_ADDRESS_OFFSET,
            offsetof(iree_hal_amdgpu_asan_report_t, fault_address));
  EXPECT_EQ(LOOM_AMDGPU_ASAN_REPORT_ACCESS_SIZE_OFFSET,
            offsetof(iree_hal_amdgpu_asan_report_t, access_size));
  EXPECT_EQ(LOOM_AMDGPU_ASAN_REPORT_SITE_ID_OFFSET,
            offsetof(iree_hal_amdgpu_asan_report_t, site_id));
  EXPECT_EQ(LOOM_AMDGPU_ASAN_REPORT_SHADOW_ADDRESS_OFFSET,
            offsetof(iree_hal_amdgpu_asan_report_t, shadow_address));
  EXPECT_EQ(LOOM_AMDGPU_ASAN_REPORT_SHADOW_VALUE_OFFSET,
            offsetof(iree_hal_amdgpu_asan_report_t, shadow_value));
  EXPECT_EQ(LOOM_AMDGPU_ASAN_REPORT_RESERVED_ARRAY_0_OFFSET,
            offsetof(iree_hal_amdgpu_asan_report_t, reserved[0]));
  EXPECT_EQ(static_cast<uint32_t>(LOOM_AMDGPU_ASAN_ACCESS_KIND_UNKNOWN),
            static_cast<uint32_t>(IREE_HAL_AMDGPU_ASAN_ACCESS_KIND_UNKNOWN));
  EXPECT_EQ(static_cast<uint32_t>(LOOM_AMDGPU_ASAN_ACCESS_KIND_READ),
            static_cast<uint32_t>(IREE_HAL_AMDGPU_ASAN_ACCESS_KIND_READ));
  EXPECT_EQ(static_cast<uint32_t>(LOOM_AMDGPU_ASAN_ACCESS_KIND_WRITE),
            static_cast<uint32_t>(IREE_HAL_AMDGPU_ASAN_ACCESS_KIND_WRITE));
  EXPECT_EQ(static_cast<uint32_t>(LOOM_AMDGPU_ASAN_ACCESS_KIND_ATOMIC),
            static_cast<uint32_t>(IREE_HAL_AMDGPU_ASAN_ACCESS_KIND_ATOMIC));
  EXPECT_EQ(static_cast<uint32_t>(LOOM_AMDGPU_ASAN_REPORT_FLAG_NONE),
            static_cast<uint32_t>(IREE_HAL_AMDGPU_ASAN_REPORT_FLAG_NONE));
}

}  // namespace
