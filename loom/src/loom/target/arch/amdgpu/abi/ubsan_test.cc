// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/target/arch/amdgpu/abi/ubsan.h"

#include <cstddef>
#include <cstdint>

#include "iree/hal/drivers/amdgpu/abi/ubsan.h"
#include "iree/testing/gtest.h"

namespace {

TEST(AmdgpuUbsanAbiTest, ConstantsMatchHalUbsanAbi) {
  EXPECT_EQ(LOOM_AMDGPU_UBSAN_REPORT_ABI_VERSION,
            IREE_HAL_AMDGPU_UBSAN_REPORT_ABI_VERSION_0);
  EXPECT_EQ(LOOM_AMDGPU_UBSAN_REPORT_BYTE_LENGTH,
            sizeof(iree_hal_amdgpu_ubsan_report_t));
  EXPECT_EQ(LOOM_AMDGPU_UBSAN_REPORT_RECORD_LENGTH_OFFSET,
            offsetof(iree_hal_amdgpu_ubsan_report_t, record_length));
  EXPECT_EQ(LOOM_AMDGPU_UBSAN_REPORT_ABI_VERSION_OFFSET,
            offsetof(iree_hal_amdgpu_ubsan_report_t, abi_version));
  EXPECT_EQ(LOOM_AMDGPU_UBSAN_REPORT_CHECK_KIND_OFFSET,
            offsetof(iree_hal_amdgpu_ubsan_report_t, check_kind));
  EXPECT_EQ(LOOM_AMDGPU_UBSAN_REPORT_FLAGS_OFFSET,
            offsetof(iree_hal_amdgpu_ubsan_report_t, flags));
  EXPECT_EQ(LOOM_AMDGPU_UBSAN_REPORT_SITE_ID_OFFSET,
            offsetof(iree_hal_amdgpu_ubsan_report_t, site_id));
  EXPECT_EQ(LOOM_AMDGPU_UBSAN_REPORT_OPERAND0_OFFSET,
            offsetof(iree_hal_amdgpu_ubsan_report_t, operand0));
  EXPECT_EQ(LOOM_AMDGPU_UBSAN_REPORT_OPERAND1_OFFSET,
            offsetof(iree_hal_amdgpu_ubsan_report_t, operand1));
  EXPECT_EQ(LOOM_AMDGPU_UBSAN_REPORT_RESERVED_ARRAY_0_OFFSET,
            offsetof(iree_hal_amdgpu_ubsan_report_t, reserved[0]));
  EXPECT_EQ(LOOM_AMDGPU_UBSAN_REPORT_RESERVED_ARRAY_1_OFFSET,
            offsetof(iree_hal_amdgpu_ubsan_report_t, reserved[1]));
  EXPECT_EQ(LOOM_AMDGPU_UBSAN_REPORT_RESERVED_ARRAY_2_OFFSET,
            offsetof(iree_hal_amdgpu_ubsan_report_t, reserved[2]));
  EXPECT_EQ(static_cast<uint32_t>(LOOM_AMDGPU_UBSAN_CHECK_KIND_UNKNOWN),
            static_cast<uint32_t>(IREE_HAL_AMDGPU_UBSAN_CHECK_KIND_UNKNOWN));
  EXPECT_EQ(
      static_cast<uint32_t>(LOOM_AMDGPU_UBSAN_CHECK_KIND_INTEGER_OVERFLOW),
      static_cast<uint32_t>(IREE_HAL_AMDGPU_UBSAN_CHECK_KIND_INTEGER_OVERFLOW));
  EXPECT_EQ(
      static_cast<uint32_t>(LOOM_AMDGPU_UBSAN_CHECK_KIND_DIVIDE_BY_ZERO),
      static_cast<uint32_t>(IREE_HAL_AMDGPU_UBSAN_CHECK_KIND_DIVIDE_BY_ZERO));
  EXPECT_EQ(static_cast<uint32_t>(LOOM_AMDGPU_UBSAN_CHECK_KIND_ALIGNMENT),
            static_cast<uint32_t>(IREE_HAL_AMDGPU_UBSAN_CHECK_KIND_ALIGNMENT));
  EXPECT_EQ(
      static_cast<uint32_t>(LOOM_AMDGPU_UBSAN_CHECK_KIND_FLOAT_NAN_CONTRACT),
      static_cast<uint32_t>(
          IREE_HAL_AMDGPU_UBSAN_CHECK_KIND_FLOAT_NAN_CONTRACT));
  EXPECT_EQ(
      static_cast<uint32_t>(LOOM_AMDGPU_UBSAN_CHECK_KIND_UNREACHABLE),
      static_cast<uint32_t>(IREE_HAL_AMDGPU_UBSAN_CHECK_KIND_UNREACHABLE));
  EXPECT_EQ(static_cast<uint32_t>(LOOM_AMDGPU_UBSAN_CHECK_KIND_ASSERTION),
            static_cast<uint32_t>(IREE_HAL_AMDGPU_UBSAN_CHECK_KIND_ASSERTION));
  EXPECT_EQ(static_cast<uint32_t>(LOOM_AMDGPU_UBSAN_REPORT_FLAG_NONE),
            static_cast<uint32_t>(IREE_HAL_AMDGPU_UBSAN_REPORT_FLAG_NONE));
}

}  // namespace
