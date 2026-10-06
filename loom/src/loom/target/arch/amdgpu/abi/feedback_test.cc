// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/target/arch/amdgpu/abi/feedback.h"

#include <cstddef>
#include <cstdint>

#include "iree/hal/drivers/amdgpu/abi/feedback.h"
#include "iree/testing/gtest.h"

namespace {

TEST(AmdgpuFeedbackAbiTest, ConstantsMatchHalFeedbackAbi) {
  EXPECT_STREQ(LOOM_AMDGPU_FEEDBACK_CONFIG_GLOBAL_NAME,
               IREE_HAL_AMDGPU_FEEDBACK_CONFIG_GLOBAL_NAME);
  EXPECT_EQ(LOOM_AMDGPU_FEEDBACK_CONFIG_ABI_VERSION,
            IREE_HAL_AMDGPU_FEEDBACK_CONFIG_ABI_VERSION_0);
  EXPECT_EQ(LOOM_AMDGPU_FEEDBACK_CHANNEL_ABI_VERSION,
            IREE_HAL_AMDGPU_FEEDBACK_CHANNEL_ABI_VERSION_0);
  EXPECT_EQ(LOOM_AMDGPU_FEEDBACK_CONFIG_BYTE_LENGTH,
            sizeof(iree_hal_amdgpu_feedback_config_t));
  EXPECT_EQ(LOOM_AMDGPU_FEEDBACK_CONFIG_CHANNEL_BASE_OFFSET,
            offsetof(iree_hal_amdgpu_feedback_config_t, channel_base));
  EXPECT_EQ(LOOM_AMDGPU_FEEDBACK_CONFIG_NOTIFY_SIGNAL_OFFSET,
            offsetof(iree_hal_amdgpu_feedback_config_t, notify_signal));
  EXPECT_EQ(LOOM_AMDGPU_FEEDBACK_CONFIG_SOURCE_CONTEXT_OFFSET,
            offsetof(iree_hal_amdgpu_feedback_config_t, source_context));

  EXPECT_EQ(LOOM_AMDGPU_FEEDBACK_CHANNEL_BYTE_LENGTH,
            sizeof(iree_hal_amdgpu_feedback_channel_header_t));
  EXPECT_EQ(LOOM_AMDGPU_FEEDBACK_CHANNEL_RING_BASE_OFFSET,
            offsetof(iree_hal_amdgpu_feedback_channel_header_t, ring_base));
  EXPECT_EQ(LOOM_AMDGPU_FEEDBACK_CHANNEL_RING_CAPACITY_OFFSET,
            offsetof(iree_hal_amdgpu_feedback_channel_header_t, ring_capacity));
  EXPECT_EQ(LOOM_AMDGPU_FEEDBACK_CHANNEL_READ_TAIL_OFFSET,
            offsetof(iree_hal_amdgpu_feedback_channel_header_t, read_tail));
  EXPECT_EQ(
      LOOM_AMDGPU_FEEDBACK_CHANNEL_RESERVATION_HEAD_OFFSET,
      offsetof(iree_hal_amdgpu_feedback_channel_header_t, reservation_head));
  EXPECT_EQ(LOOM_AMDGPU_FEEDBACK_CHANNEL_DROPPED_PACKET_COUNT_OFFSET,
            offsetof(iree_hal_amdgpu_feedback_channel_header_t,
                     dropped_packet_count));

  EXPECT_EQ(LOOM_AMDGPU_FEEDBACK_PACKET_BYTE_LENGTH,
            sizeof(iree_hal_amdgpu_feedback_packet_t));
  EXPECT_EQ(LOOM_AMDGPU_FEEDBACK_PACKET_HEADER_LENGTH_OFFSET,
            offsetof(iree_hal_amdgpu_feedback_packet_t, header_length));
  EXPECT_EQ(LOOM_AMDGPU_FEEDBACK_PACKET_KIND_OFFSET,
            offsetof(iree_hal_amdgpu_feedback_packet_t, kind));
  EXPECT_EQ(LOOM_AMDGPU_FEEDBACK_PACKET_FLAGS_OFFSET,
            offsetof(iree_hal_amdgpu_feedback_packet_t, flags));
  EXPECT_EQ(LOOM_AMDGPU_FEEDBACK_PACKET_STATE_OFFSET,
            offsetof(iree_hal_amdgpu_feedback_packet_t, state));
  EXPECT_EQ(LOOM_AMDGPU_FEEDBACK_PACKET_SEQUENCE_OFFSET,
            offsetof(iree_hal_amdgpu_feedback_packet_t, sequence));
  EXPECT_EQ(LOOM_AMDGPU_FEEDBACK_PACKET_SOURCE_DISPATCH_PTR_OFFSET,
            offsetof(iree_hal_amdgpu_feedback_packet_t, source_dispatch_ptr));
  EXPECT_EQ(LOOM_AMDGPU_FEEDBACK_PACKET_SOURCE_WORKGROUP_ID_X_OFFSET,
            offsetof(iree_hal_amdgpu_feedback_packet_t, source_workgroup_id_x));
  EXPECT_EQ(LOOM_AMDGPU_FEEDBACK_PACKET_SOURCE_WORKITEM_ID_X_OFFSET,
            offsetof(iree_hal_amdgpu_feedback_packet_t, source_workitem_id_x));
  EXPECT_EQ(LOOM_AMDGPU_FEEDBACK_PACKET_SOURCE_CONTEXT_OFFSET,
            offsetof(iree_hal_amdgpu_feedback_packet_t, source_context));
  EXPECT_EQ(LOOM_AMDGPU_FEEDBACK_PACKET_RESERVED_ARRAY_0_OFFSET,
            offsetof(iree_hal_amdgpu_feedback_packet_t, reserved[0]));
  EXPECT_EQ(LOOM_AMDGPU_FEEDBACK_PACKET_RESERVED_ARRAY_1_OFFSET,
            offsetof(iree_hal_amdgpu_feedback_packet_t, reserved[1]));
}

TEST(AmdgpuFeedbackAbiTest, PacketLengthRoundsUpToAbiAlignment) {
  EXPECT_EQ(LOOM_AMDGPU_FEEDBACK_PACKET_ALIGNMENT,
            IREE_HAL_AMDGPU_FEEDBACK_PACKET_ALIGNMENT);
  EXPECT_EQ(LOOM_AMDGPU_FEEDBACK_PACKET_MAX_PAYLOAD_LENGTH,
            IREE_HAL_AMDGPU_FEEDBACK_PACKET_MAX_PAYLOAD_LENGTH);
  EXPECT_EQ(loom_amdgpu_feedback_packet_length(0), 64u);
  EXPECT_EQ(loom_amdgpu_feedback_packet_length(1), 128u);
  EXPECT_EQ(loom_amdgpu_feedback_packet_length(63), 128u);
  EXPECT_EQ(loom_amdgpu_feedback_packet_length(64), 128u);
  EXPECT_EQ(loom_amdgpu_feedback_packet_length(65), 192u);
  EXPECT_EQ(loom_amdgpu_feedback_packet_length(
                LOOM_AMDGPU_FEEDBACK_PACKET_MAX_PAYLOAD_LENGTH),
            64u + 16u * 1024u);
  EXPECT_EQ(loom_amdgpu_feedback_packet_length(65),
            iree_hal_amdgpu_feedback_packet_length(65));
}

TEST(AmdgpuFeedbackAbiTest, PacketKindsReserveCommonRuntimeSchemas) {
  EXPECT_EQ(static_cast<uint32_t>(LOOM_AMDGPU_FEEDBACK_PACKET_KIND_NONE),
            static_cast<uint32_t>(IREE_HAL_AMDGPU_FEEDBACK_PACKET_KIND_NONE));
  EXPECT_EQ(static_cast<uint32_t>(LOOM_AMDGPU_FEEDBACK_PACKET_KIND_ASAN),
            static_cast<uint32_t>(IREE_HAL_AMDGPU_FEEDBACK_PACKET_KIND_ASAN));
  EXPECT_EQ(static_cast<uint32_t>(LOOM_AMDGPU_FEEDBACK_PACKET_KIND_PRINTF),
            static_cast<uint32_t>(IREE_HAL_AMDGPU_FEEDBACK_PACKET_KIND_PRINTF));
  EXPECT_EQ(
      static_cast<uint32_t>(LOOM_AMDGPU_FEEDBACK_PACKET_KIND_HOST_CALL),
      static_cast<uint32_t>(IREE_HAL_AMDGPU_FEEDBACK_PACKET_KIND_HOST_CALL));
  EXPECT_EQ(static_cast<uint32_t>(LOOM_AMDGPU_FEEDBACK_PACKET_KIND_TSAN),
            static_cast<uint32_t>(IREE_HAL_AMDGPU_FEEDBACK_PACKET_KIND_TSAN));
  EXPECT_EQ(static_cast<uint32_t>(LOOM_AMDGPU_FEEDBACK_PACKET_KIND_UBSAN),
            static_cast<uint32_t>(IREE_HAL_AMDGPU_FEEDBACK_PACKET_KIND_UBSAN));
  EXPECT_EQ(static_cast<uint32_t>(LOOM_AMDGPU_FEEDBACK_PACKET_KIND_USER),
            static_cast<uint32_t>(IREE_HAL_AMDGPU_FEEDBACK_PACKET_KIND_USER));
}

TEST(AmdgpuFeedbackAbiTest, FlagsAndStatesMatchHalFeedbackAbi) {
  EXPECT_EQ(static_cast<uint32_t>(LOOM_AMDGPU_FEEDBACK_CONFIG_FLAG_NONE),
            static_cast<uint32_t>(IREE_HAL_AMDGPU_FEEDBACK_CONFIG_FLAG_NONE));
  EXPECT_EQ(
      static_cast<uint32_t>(LOOM_AMDGPU_FEEDBACK_CONFIG_FLAG_ENABLED),
      static_cast<uint32_t>(IREE_HAL_AMDGPU_FEEDBACK_CONFIG_FLAG_ENABLED));
  EXPECT_EQ(
      static_cast<uint32_t>(LOOM_AMDGPU_FEEDBACK_PACKET_STATE_RESERVED),
      static_cast<uint32_t>(IREE_HAL_AMDGPU_FEEDBACK_PACKET_STATE_RESERVED));
  EXPECT_EQ(static_cast<uint32_t>(LOOM_AMDGPU_FEEDBACK_PACKET_STATE_READY),
            static_cast<uint32_t>(IREE_HAL_AMDGPU_FEEDBACK_PACKET_STATE_READY));
  EXPECT_EQ(static_cast<uint32_t>(LOOM_AMDGPU_FEEDBACK_PACKET_FLAG_NONE),
            static_cast<uint32_t>(IREE_HAL_AMDGPU_FEEDBACK_PACKET_FLAG_NONE));
  EXPECT_EQ(static_cast<uint32_t>(LOOM_AMDGPU_FEEDBACK_PACKET_FLAG_ASYNC),
            static_cast<uint32_t>(IREE_HAL_AMDGPU_FEEDBACK_PACKET_FLAG_ASYNC));
}

}  // namespace
