// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/target/arch/amdgpu/abi/tsan.h"

#include <cstddef>
#include <cstdint>

#include "iree/hal/drivers/amdgpu/abi/tsan.h"
#include "iree/testing/gtest.h"

namespace {

TEST(AmdgpuTsanAbiTest, ConfigConstantsMatchHalTsanAbi) {
  EXPECT_STREQ(LOOM_AMDGPU_TSAN_CONFIG_GLOBAL_NAME,
               IREE_HAL_AMDGPU_TSAN_CONFIG_GLOBAL_NAME);
  EXPECT_EQ(LOOM_AMDGPU_TSAN_CONFIG_BYTE_LENGTH,
            sizeof(iree_hal_amdgpu_tsan_config_t));
  EXPECT_EQ(LOOM_AMDGPU_TSAN_CONFIG_FLAGS_OFFSET,
            offsetof(iree_hal_amdgpu_tsan_config_t, flags));
  EXPECT_EQ(LOOM_AMDGPU_TSAN_CONFIG_MEMORY_GRANULE_SHIFT_OFFSET,
            offsetof(iree_hal_amdgpu_tsan_config_t, memory_granule_shift));
  EXPECT_EQ(LOOM_AMDGPU_TSAN_CONFIG_SHADOW_BASE_OFFSET,
            offsetof(iree_hal_amdgpu_tsan_config_t, shadow_base));
  EXPECT_EQ(LOOM_AMDGPU_TSAN_CONFIG_DISPATCH_SHADOW_STRIDE_OFFSET,
            offsetof(iree_hal_amdgpu_tsan_config_t, dispatch_shadow_stride));
  EXPECT_EQ(LOOM_AMDGPU_TSAN_CONFIG_WORKGROUP_SHADOW_STRIDE_OFFSET,
            offsetof(iree_hal_amdgpu_tsan_config_t, workgroup_shadow_stride));
  EXPECT_EQ(LOOM_AMDGPU_TSAN_CONFIG_WORKGROUP_CAPACITY_OFFSET,
            offsetof(iree_hal_amdgpu_tsan_config_t, workgroup_capacity));
  EXPECT_EQ(LOOM_AMDGPU_TSAN_CONFIG_SHADOW_ENTRY_SIZE_OFFSET,
            offsetof(iree_hal_amdgpu_tsan_config_t, shadow_entry_size));
  EXPECT_EQ(LOOM_AMDGPU_TSAN_CONFIG_QUEUE_AQL_BASE_OFFSET,
            offsetof(iree_hal_amdgpu_tsan_config_t, queue_aql_base));
  EXPECT_EQ(LOOM_AMDGPU_TSAN_CONFIG_QUEUE_AQL_SLOT_MASK_OFFSET,
            offsetof(iree_hal_amdgpu_tsan_config_t, queue_aql_slot_mask));
  EXPECT_EQ(LOOM_AMDGPU_TSAN_CONFIG_QUEUE_STATE_BASE_OFFSET,
            offsetof(iree_hal_amdgpu_tsan_config_t, queue_state_base));
  EXPECT_EQ(LOOM_AMDGPU_TSAN_CONFIG_SHADOW_SLOT_COUNT_OFFSET,
            offsetof(iree_hal_amdgpu_tsan_config_t, shadow_slot_count));
  EXPECT_EQ(static_cast<uint32_t>(LOOM_AMDGPU_TSAN_CONFIG_FLAG_ENABLED),
            static_cast<uint32_t>(IREE_HAL_AMDGPU_TSAN_CONFIG_FLAG_ENABLED));
  EXPECT_EQ(
      static_cast<uint32_t>(LOOM_AMDGPU_TSAN_CONFIG_FLAG_QUEUE_STATE),
      static_cast<uint32_t>(IREE_HAL_AMDGPU_TSAN_CONFIG_FLAG_QUEUE_STATE));
}

TEST(AmdgpuTsanAbiTest, QueueStateConstantsMatchHalTsanAbi) {
  EXPECT_EQ(LOOM_AMDGPU_TSAN_QUEUE_STATE_BYTE_LENGTH,
            sizeof(iree_hal_amdgpu_tsan_queue_state_t));
  EXPECT_EQ(LOOM_AMDGPU_TSAN_QUEUE_STATE_SHADOW_BASE_OFFSET,
            offsetof(iree_hal_amdgpu_tsan_queue_state_t, shadow_base));
  EXPECT_EQ(LOOM_AMDGPU_TSAN_QUEUE_STATE_SHADOW_SIZE_OFFSET,
            offsetof(iree_hal_amdgpu_tsan_queue_state_t, shadow_size));
  EXPECT_EQ(
      LOOM_AMDGPU_TSAN_QUEUE_STATE_DISPATCH_SHADOW_STRIDE_OFFSET,
      offsetof(iree_hal_amdgpu_tsan_queue_state_t, dispatch_shadow_stride));
  EXPECT_EQ(
      LOOM_AMDGPU_TSAN_QUEUE_STATE_WORKGROUP_SHADOW_STRIDE_OFFSET,
      offsetof(iree_hal_amdgpu_tsan_queue_state_t, workgroup_shadow_stride));
  EXPECT_EQ(LOOM_AMDGPU_TSAN_QUEUE_STATE_WORKGROUP_CAPACITY_OFFSET,
            offsetof(iree_hal_amdgpu_tsan_queue_state_t, workgroup_capacity));
  EXPECT_EQ(LOOM_AMDGPU_TSAN_QUEUE_STATE_SHADOW_ENTRY_SIZE_OFFSET,
            offsetof(iree_hal_amdgpu_tsan_queue_state_t, shadow_entry_size));
  EXPECT_EQ(LOOM_AMDGPU_TSAN_QUEUE_STATE_MEMORY_GRANULE_SHIFT_OFFSET,
            offsetof(iree_hal_amdgpu_tsan_queue_state_t, memory_granule_shift));
}

TEST(AmdgpuTsanAbiTest, ShadowConstantsMatchHalTsanAbi) {
  EXPECT_EQ(
      static_cast<uint32_t>(LOOM_AMDGPU_TSAN_SHADOW_ENTRY_BYTE_LENGTH),
      static_cast<uint32_t>(IREE_HAL_AMDGPU_TSAN_SHADOW_ENTRY_BYTE_LENGTH));
  EXPECT_EQ(static_cast<uint32_t>(
                LOOM_AMDGPU_TSAN_WORKGROUP_SHADOW_HEADER_BYTE_LENGTH),
            static_cast<uint32_t>(
                IREE_HAL_AMDGPU_TSAN_WORKGROUP_SHADOW_HEADER_BYTE_LENGTH));
  EXPECT_EQ(
      static_cast<uint32_t>(LOOM_AMDGPU_TSAN_WORKGROUP_SHADOW_EPOCH_OFFSET),
      static_cast<uint32_t>(
          IREE_HAL_AMDGPU_TSAN_WORKGROUP_SHADOW_EPOCH_OFFSET));
  EXPECT_EQ(
      static_cast<uint32_t>(LOOM_AMDGPU_TSAN_SHADOW_ENTRY_ACCESS_KIND_SHIFT),
      static_cast<uint32_t>(
          IREE_HAL_AMDGPU_TSAN_SHADOW_ENTRY_ACCESS_KIND_SHIFT));
  EXPECT_EQ(static_cast<uint32_t>(
                LOOM_AMDGPU_TSAN_SHADOW_ENTRY_ACCESS_KIND_BIT_COUNT),
            static_cast<uint32_t>(
                IREE_HAL_AMDGPU_TSAN_SHADOW_ENTRY_ACCESS_KIND_BIT_COUNT));
  EXPECT_EQ(
      static_cast<uint32_t>(LOOM_AMDGPU_TSAN_SHADOW_ENTRY_WORKITEM_SHIFT),
      static_cast<uint32_t>(IREE_HAL_AMDGPU_TSAN_SHADOW_ENTRY_WORKITEM_SHIFT));
  EXPECT_EQ(
      static_cast<uint32_t>(LOOM_AMDGPU_TSAN_SHADOW_ENTRY_WORKITEM_BIT_COUNT),
      static_cast<uint32_t>(
          IREE_HAL_AMDGPU_TSAN_SHADOW_ENTRY_WORKITEM_BIT_COUNT));
  EXPECT_EQ(
      static_cast<uint32_t>(LOOM_AMDGPU_TSAN_SHADOW_ENTRY_EPOCH_SHIFT),
      static_cast<uint32_t>(IREE_HAL_AMDGPU_TSAN_SHADOW_ENTRY_EPOCH_SHIFT));
  EXPECT_EQ(
      static_cast<uint32_t>(LOOM_AMDGPU_TSAN_SHADOW_ENTRY_EPOCH_BIT_COUNT),
      static_cast<uint32_t>(IREE_HAL_AMDGPU_TSAN_SHADOW_ENTRY_EPOCH_BIT_COUNT));
  EXPECT_EQ(
      static_cast<uint32_t>(LOOM_AMDGPU_TSAN_SHADOW_ENTRY_GENERATION_SHIFT),
      static_cast<uint32_t>(
          IREE_HAL_AMDGPU_TSAN_SHADOW_ENTRY_GENERATION_SHIFT));
  EXPECT_EQ(
      static_cast<uint32_t>(LOOM_AMDGPU_TSAN_SHADOW_ENTRY_GENERATION_BIT_COUNT),
      static_cast<uint32_t>(
          IREE_HAL_AMDGPU_TSAN_SHADOW_ENTRY_GENERATION_BIT_COUNT));
  EXPECT_EQ(
      static_cast<uint32_t>(LOOM_AMDGPU_TSAN_SHADOW_ENTRY_SITE_SHIFT),
      static_cast<uint32_t>(IREE_HAL_AMDGPU_TSAN_SHADOW_ENTRY_SITE_SHIFT));
  EXPECT_EQ(
      static_cast<uint32_t>(LOOM_AMDGPU_TSAN_SHADOW_ENTRY_SITE_BIT_COUNT),
      static_cast<uint32_t>(IREE_HAL_AMDGPU_TSAN_SHADOW_ENTRY_SITE_BIT_COUNT));
  EXPECT_EQ(
      static_cast<uint32_t>(LOOM_AMDGPU_TSAN_SHADOW_ACCESS_KIND_EMPTY),
      static_cast<uint32_t>(IREE_HAL_AMDGPU_TSAN_SHADOW_ACCESS_KIND_EMPTY));
  EXPECT_EQ(
      static_cast<uint32_t>(LOOM_AMDGPU_TSAN_SHADOW_ACCESS_KIND_READ),
      static_cast<uint32_t>(IREE_HAL_AMDGPU_TSAN_SHADOW_ACCESS_KIND_READ));
  EXPECT_EQ(
      static_cast<uint32_t>(LOOM_AMDGPU_TSAN_SHADOW_ACCESS_KIND_WRITE),
      static_cast<uint32_t>(IREE_HAL_AMDGPU_TSAN_SHADOW_ACCESS_KIND_WRITE));
  EXPECT_EQ(
      static_cast<uint32_t>(LOOM_AMDGPU_TSAN_SHADOW_ACCESS_KIND_READ_WRITE),
      static_cast<uint32_t>(
          IREE_HAL_AMDGPU_TSAN_SHADOW_ACCESS_KIND_READ_WRITE));
  EXPECT_EQ(
      static_cast<uint32_t>(LOOM_AMDGPU_TSAN_SHADOW_ACCESS_KIND_ATOMIC),
      static_cast<uint32_t>(IREE_HAL_AMDGPU_TSAN_SHADOW_ACCESS_KIND_ATOMIC));
}

TEST(AmdgpuTsanAbiTest, ReportConstantsMatchHalTsanAbi) {
  EXPECT_EQ(LOOM_AMDGPU_TSAN_REPORT_ABI_VERSION,
            IREE_HAL_AMDGPU_TSAN_REPORT_ABI_VERSION_0);
  EXPECT_EQ(LOOM_AMDGPU_TSAN_REPORT_BYTE_LENGTH,
            sizeof(iree_hal_amdgpu_tsan_report_t));
  EXPECT_EQ(LOOM_AMDGPU_TSAN_REPORT_RECORD_LENGTH_OFFSET,
            offsetof(iree_hal_amdgpu_tsan_report_t, record_length));
  EXPECT_EQ(LOOM_AMDGPU_TSAN_REPORT_ABI_VERSION_OFFSET,
            offsetof(iree_hal_amdgpu_tsan_report_t, abi_version));
  EXPECT_EQ(LOOM_AMDGPU_TSAN_REPORT_CHECK_KIND_OFFSET,
            offsetof(iree_hal_amdgpu_tsan_report_t, check_kind));
  EXPECT_EQ(LOOM_AMDGPU_TSAN_REPORT_FLAGS_OFFSET,
            offsetof(iree_hal_amdgpu_tsan_report_t, flags));
  EXPECT_EQ(LOOM_AMDGPU_TSAN_REPORT_MEMORY_SPACE_OFFSET,
            offsetof(iree_hal_amdgpu_tsan_report_t, memory_space));
  EXPECT_EQ(LOOM_AMDGPU_TSAN_REPORT_CURRENT_ACCESS_KIND_OFFSET,
            offsetof(iree_hal_amdgpu_tsan_report_t, current_access_kind));
  EXPECT_EQ(LOOM_AMDGPU_TSAN_REPORT_PRIOR_ACCESS_KIND_OFFSET,
            offsetof(iree_hal_amdgpu_tsan_report_t, prior_access_kind));
  EXPECT_EQ(LOOM_AMDGPU_TSAN_REPORT_ACCESS_SIZE_OFFSET,
            offsetof(iree_hal_amdgpu_tsan_report_t, access_size));
  EXPECT_EQ(LOOM_AMDGPU_TSAN_REPORT_CURRENT_SITE_ID_OFFSET,
            offsetof(iree_hal_amdgpu_tsan_report_t, current_site_id));
  EXPECT_EQ(LOOM_AMDGPU_TSAN_REPORT_PRIOR_SITE_ID_OFFSET,
            offsetof(iree_hal_amdgpu_tsan_report_t, prior_site_id));
  EXPECT_EQ(LOOM_AMDGPU_TSAN_REPORT_MEMORY_ADDRESS_OFFSET,
            offsetof(iree_hal_amdgpu_tsan_report_t, memory_address));
  EXPECT_EQ(LOOM_AMDGPU_TSAN_REPORT_SHADOW_ADDRESS_OFFSET,
            offsetof(iree_hal_amdgpu_tsan_report_t, shadow_address));
  EXPECT_EQ(LOOM_AMDGPU_TSAN_REPORT_SHADOW_VALUE_OFFSET,
            offsetof(iree_hal_amdgpu_tsan_report_t, shadow_value));
  EXPECT_EQ(LOOM_AMDGPU_TSAN_REPORT_CURRENT_WORKGROUP_ID_X_OFFSET,
            offsetof(iree_hal_amdgpu_tsan_report_t, current_workgroup_id[0]));
  EXPECT_EQ(LOOM_AMDGPU_TSAN_REPORT_CURRENT_WORKGROUP_ID_Y_OFFSET,
            offsetof(iree_hal_amdgpu_tsan_report_t, current_workgroup_id[1]));
  EXPECT_EQ(LOOM_AMDGPU_TSAN_REPORT_CURRENT_WORKGROUP_ID_Z_OFFSET,
            offsetof(iree_hal_amdgpu_tsan_report_t, current_workgroup_id[2]));
  EXPECT_EQ(LOOM_AMDGPU_TSAN_REPORT_CURRENT_WORKITEM_ID_X_OFFSET,
            offsetof(iree_hal_amdgpu_tsan_report_t, current_workitem_id[0]));
  EXPECT_EQ(LOOM_AMDGPU_TSAN_REPORT_CURRENT_WORKITEM_ID_Y_OFFSET,
            offsetof(iree_hal_amdgpu_tsan_report_t, current_workitem_id[1]));
  EXPECT_EQ(LOOM_AMDGPU_TSAN_REPORT_CURRENT_WORKITEM_ID_Z_OFFSET,
            offsetof(iree_hal_amdgpu_tsan_report_t, current_workitem_id[2]));
  EXPECT_EQ(LOOM_AMDGPU_TSAN_REPORT_PRIOR_WORKGROUP_ID_X_OFFSET,
            offsetof(iree_hal_amdgpu_tsan_report_t, prior_workgroup_id[0]));
  EXPECT_EQ(LOOM_AMDGPU_TSAN_REPORT_PRIOR_WORKGROUP_ID_Y_OFFSET,
            offsetof(iree_hal_amdgpu_tsan_report_t, prior_workgroup_id[1]));
  EXPECT_EQ(LOOM_AMDGPU_TSAN_REPORT_PRIOR_WORKGROUP_ID_Z_OFFSET,
            offsetof(iree_hal_amdgpu_tsan_report_t, prior_workgroup_id[2]));
  EXPECT_EQ(LOOM_AMDGPU_TSAN_REPORT_PRIOR_WORKITEM_ID_X_OFFSET,
            offsetof(iree_hal_amdgpu_tsan_report_t, prior_workitem_id[0]));
  EXPECT_EQ(LOOM_AMDGPU_TSAN_REPORT_PRIOR_WORKITEM_ID_Y_OFFSET,
            offsetof(iree_hal_amdgpu_tsan_report_t, prior_workitem_id[1]));
  EXPECT_EQ(LOOM_AMDGPU_TSAN_REPORT_PRIOR_WORKITEM_ID_Z_OFFSET,
            offsetof(iree_hal_amdgpu_tsan_report_t, prior_workitem_id[2]));
  EXPECT_EQ(static_cast<uint32_t>(LOOM_AMDGPU_TSAN_CHECK_KIND_UNKNOWN),
            static_cast<uint32_t>(IREE_HAL_AMDGPU_TSAN_CHECK_KIND_UNKNOWN));
  EXPECT_EQ(static_cast<uint32_t>(LOOM_AMDGPU_TSAN_CHECK_KIND_DATA_RACE),
            static_cast<uint32_t>(IREE_HAL_AMDGPU_TSAN_CHECK_KIND_DATA_RACE));
  EXPECT_EQ(static_cast<uint32_t>(LOOM_AMDGPU_TSAN_MEMORY_SPACE_UNKNOWN),
            static_cast<uint32_t>(IREE_HAL_AMDGPU_TSAN_MEMORY_SPACE_UNKNOWN));
  EXPECT_EQ(static_cast<uint32_t>(LOOM_AMDGPU_TSAN_MEMORY_SPACE_GLOBAL),
            static_cast<uint32_t>(IREE_HAL_AMDGPU_TSAN_MEMORY_SPACE_GLOBAL));
  EXPECT_EQ(static_cast<uint32_t>(LOOM_AMDGPU_TSAN_MEMORY_SPACE_WORKGROUP),
            static_cast<uint32_t>(IREE_HAL_AMDGPU_TSAN_MEMORY_SPACE_WORKGROUP));
  EXPECT_EQ(static_cast<uint32_t>(LOOM_AMDGPU_TSAN_MEMORY_SPACE_PRIVATE),
            static_cast<uint32_t>(IREE_HAL_AMDGPU_TSAN_MEMORY_SPACE_PRIVATE));
  EXPECT_EQ(static_cast<uint32_t>(LOOM_AMDGPU_TSAN_ACCESS_KIND_UNKNOWN),
            static_cast<uint32_t>(IREE_HAL_AMDGPU_TSAN_ACCESS_KIND_UNKNOWN));
  EXPECT_EQ(static_cast<uint32_t>(LOOM_AMDGPU_TSAN_ACCESS_KIND_READ),
            static_cast<uint32_t>(IREE_HAL_AMDGPU_TSAN_ACCESS_KIND_READ));
  EXPECT_EQ(static_cast<uint32_t>(LOOM_AMDGPU_TSAN_ACCESS_KIND_WRITE),
            static_cast<uint32_t>(IREE_HAL_AMDGPU_TSAN_ACCESS_KIND_WRITE));
  EXPECT_EQ(static_cast<uint32_t>(LOOM_AMDGPU_TSAN_ACCESS_KIND_READ_WRITE),
            static_cast<uint32_t>(IREE_HAL_AMDGPU_TSAN_ACCESS_KIND_READ_WRITE));
  EXPECT_EQ(static_cast<uint32_t>(LOOM_AMDGPU_TSAN_ACCESS_KIND_ATOMIC),
            static_cast<uint32_t>(IREE_HAL_AMDGPU_TSAN_ACCESS_KIND_ATOMIC));
  EXPECT_EQ(static_cast<uint32_t>(LOOM_AMDGPU_TSAN_REPORT_FLAG_NONE),
            static_cast<uint32_t>(IREE_HAL_AMDGPU_TSAN_REPORT_FLAG_NONE));
  EXPECT_EQ(
      static_cast<uint32_t>(LOOM_AMDGPU_TSAN_REPORT_FLAG_CURRENT_ATOMIC),
      static_cast<uint32_t>(IREE_HAL_AMDGPU_TSAN_REPORT_FLAG_CURRENT_ATOMIC));
  EXPECT_EQ(
      static_cast<uint32_t>(LOOM_AMDGPU_TSAN_REPORT_FLAG_PRIOR_ATOMIC),
      static_cast<uint32_t>(IREE_HAL_AMDGPU_TSAN_REPORT_FLAG_PRIOR_ATOMIC));
  EXPECT_EQ(
      static_cast<uint32_t>(LOOM_AMDGPU_TSAN_REPORT_FLAG_PRIOR_WORKITEM_LINEAR),
      static_cast<uint32_t>(
          IREE_HAL_AMDGPU_TSAN_REPORT_FLAG_PRIOR_WORKITEM_LINEAR));
  EXPECT_EQ(static_cast<uint32_t>(
                LOOM_AMDGPU_TSAN_REPORT_FLAG_CURRENT_WORKITEM_LINEAR),
            static_cast<uint32_t>(
                IREE_HAL_AMDGPU_TSAN_REPORT_FLAG_CURRENT_WORKITEM_LINEAR));
}

}  // namespace
