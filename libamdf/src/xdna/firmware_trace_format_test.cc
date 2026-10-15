// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "libamdf/src/xdna/firmware_trace_format.h"

#include <array>
#include <cstring>

#include "gtest/gtest.h"

namespace {

// Native format-2 frame carrying EXEC_NPU_CMD on physical context 15 and
// virtual context 3. Header/footer framing is independent of event arguments.
std::array<uint64_t, 4> Frame(uint64_t position, uint64_t timestamp,
                              uint32_t event_id) {
  const uint64_t sequence = position / 32 % 65535 + 1;
  return {UINT64_C(0x02ca) | (sequence << 16), timestamp,
          (uint64_t{event_id} << 48) | UINT64_C(0x30017f),
          (UINT64_C(0xba02) << 48) | (sequence << 32)};
}

TEST(FirmwareTraceFormatTest, CompactsCompleteFramesAcrossSequenceWrap) {
  constexpr uint64_t kBegin = 65534 * 32;
  std::array<uint64_t, 13 + 12> storage = {};
  const auto first = Frame(kBegin, 1000, 0);
  const auto second = Frame(kBegin + 32, 1100, 1);
  const auto third = Frame(kBegin + 64, 1200, 0x1234);
  std::memcpy(storage.data() + 13, first.data(), 32);
  std::memcpy(storage.data() + 17, second.data(), 32);
  std::memcpy(storage.data() + 21, third.data(), 32);
  amdf_observation_read_result_t result = {
      .next_position = kBegin + 96,
      .byte_offset = 104,
      .byte_length = 96,
  };
  amdf_xdna_firmware_trace_decode(kBegin, storage.data(), &result);
  ASSERT_EQ(result.flags, 0u);
  ASSERT_EQ(result.next_position, kBegin + 96);
  ASSERT_EQ(result.byte_length, 3 * sizeof(amdf_xdna_firmware_trace_event_t));
  std::array<amdf_xdna_firmware_trace_event_t, 3> events;
  std::memcpy(events.data(), storage.data() + 13, sizeof(events));
  EXPECT_EQ(events[0].sequence, 65535u);
  EXPECT_EQ(events[1].sequence, 1u);
  EXPECT_EQ(events[2].sequence, 2u);
  EXPECT_EQ(events[0].timestamp, 1000u);
  EXPECT_EQ(events[1].timestamp, 1100u);
  EXPECT_EQ(events[2].event_id, 0x1234u);
  EXPECT_EQ(amdf_xdna_firmware_trace_command_context(&events[0]), 15u);
  EXPECT_EQ(amdf_xdna_firmware_trace_command_virtual_context(&events[0]), 3u);
  EXPECT_EQ(amdf_xdna_firmware_trace_command_opcode(&events[0]), 23u);
}

TEST(FirmwareTraceFormatTest, RetainsNativeLossBeforeSurvivingEvents) {
  auto frame = Frame(128, 1000, 1);
  amdf_observation_read_result_t result = {
      .next_position = 160,
      .byte_length = 32,
      .lost_byte_length = 96,
      .flags = AMDF_OBSERVATION_READ_FLAG_GAP,
  };
  amdf_xdna_firmware_trace_decode(32, frame.data(), &result);
  EXPECT_EQ(result.flags, AMDF_OBSERVATION_READ_FLAG_GAP);
  EXPECT_EQ(result.lost_byte_length, 96u);
  EXPECT_EQ(result.byte_length, sizeof(amdf_xdna_firmware_trace_event_t));
}

TEST(FirmwareTraceFormatTest, RejectsCompleteFrameFromAnOverwrittenGeneration) {
  // Both framing words are individually valid and agree, but this is a newer
  // generation than the native position says was copied.
  auto frame = Frame(32, 1000, 0);
  amdf_observation_read_result_t result = {
      .next_position = 32,
      .byte_length = 32,
  };
  amdf_xdna_firmware_trace_decode(0, frame.data(), &result);
  EXPECT_EQ(result.byte_length, 0u);
  EXPECT_EQ(result.lost_byte_length, 32u);
  EXPECT_EQ(result.flags, AMDF_OBSERVATION_READ_FLAG_GAP |
                              AMDF_OBSERVATION_READ_FLAG_DISCARDED);
}

TEST(FirmwareTraceFormatTest, RejectsEntireBatchWhenFooterIsNotPublished) {
  std::array<uint64_t, 8> storage;
  const auto first = Frame(0, 1000, 0);
  auto second = Frame(32, 1100, 1);
  second[3] = first[3];
  std::memcpy(storage.data(), first.data(), 32);
  std::memcpy(storage.data() + 4, second.data(), 32);
  const auto before = storage;
  amdf_observation_read_result_t result = {
      .next_position = 64,
      .byte_length = 64,
  };
  amdf_xdna_firmware_trace_decode(0, storage.data(), &result);
  EXPECT_EQ(result.byte_length, 0u);
  EXPECT_EQ(result.lost_byte_length, 64u);
  EXPECT_EQ(storage, before);
}

TEST(FirmwareTraceFormatTest, EmptySnapshotAndRestartPreserveProgressMeaning) {
  uint64_t storage = UINT64_MAX;
  amdf_observation_read_result_t empty = {.next_position = 64};
  amdf_xdna_firmware_trace_decode(64, &storage, &empty);
  EXPECT_EQ(empty.next_position, 64u);
  EXPECT_EQ(empty.flags, 0u);
  amdf_observation_read_result_t restart = {
      .lost_byte_length = UINT64_MAX,
      .flags =
          AMDF_OBSERVATION_READ_FLAG_GAP | AMDF_OBSERVATION_READ_FLAG_RESTART,
  };
  amdf_xdna_firmware_trace_decode(64, &storage, &restart);
  EXPECT_EQ(restart.next_position, 0u);
  EXPECT_EQ(restart.lost_byte_length, UINT64_MAX);
  EXPECT_EQ(storage, UINT64_MAX);
}

}  // namespace
