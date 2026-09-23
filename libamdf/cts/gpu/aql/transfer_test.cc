// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#include "libamdf/cts/gpu/aql/queue_fixture.h"
#include "libamdf/cts/gpu/pm4/encoding/memory_commands.h"

namespace {

class AqlTransferTest
    : public AqlQueueTest,
      public ::testing::WithParamInterface<pm4::CopyDataWidth> {
 protected:
  AqlTransferTest()
      : AqlQueueTest(AMDF_QUEUE_ROLE_TRANSFER | AMDF_QUEUE_ROLE_CACHE_CONTROL) {
  }
};

TEST_P(AqlTransferTest, ConfirmedWriteFeedsCopyAcrossEpochs) {
  if ((features_ & AMDF_GPU_DEVICE_FEATURE_LOCAL_MEMORY) == 0) {
    GTEST_SKIP() << "AQL transfers require the discrete coherent SYSTEM memory "
                    "policy";
  }
  constexpr size_t kByteLength = 4096;
  constexpr size_t kWordCount = kByteLength / sizeof(uint32_t);
  constexpr size_t kPayloadByteOffset = 64;
  constexpr size_t kPayloadWordOffset = kPayloadByteOffset / sizeof(uint32_t);
  constexpr size_t kCommandByteStride = 64;
  constexpr size_t kCompletionGuardWordCount =
      (kByteLength - sizeof(aql::Signal)) / sizeof(uint32_t);
  constexpr uint32_t kSourceGuard = 0x759bf13du;
  constexpr uint32_t kTargetGuard = 0x4e90b725u;
  constexpr uint32_t kCompletionGuard = 0x68d329b7u;
  constexpr uint64_t kIbAddressLimit = UINT64_C(1) << 48;
  constexpr std::array<std::array<uint32_t, 2>, 2> kPayloads = {
      {{0x13579bdfu, 0x2468ace0u}, {0xfdb97531u, 0x80a6c42eu}}};
  constexpr aql::FenceScopes kScopes = {aql::FenceScope::kSystem,
                                        aql::FenceScope::kSystem};
  static_assert(14 * sizeof(uint32_t) <= kCommandByteStride);
  static_assert(kPayloads.size() * kCommandByteStride <= kByteLength);
  const pm4::CopyDataWidth width = GetParam();
  const uint32_t payload_word_count =
      width == pm4::CopyDataWidth::k32Bit ? 1 : 2;
  // WRITE_DATA has four prefix words and COPY_DATA has six total words.
  const uint32_t body_word_count = 10 + payload_word_count;
  const uint32_t ib_word_count = 2 + body_word_count;

  GpuMemory* source = nullptr;
  GpuMemory* target = nullptr;
  GpuMemory* commands = nullptr;
  GpuMemory* completion = nullptr;
  ASSERT_NO_FATAL_FAILURE(
      CreateMemory(AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE,
                   kByteLength, &source));
  ASSERT_NO_FATAL_FAILURE(
      CreateMemory(AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE,
                   kByteLength, &target));
  ASSERT_NO_FATAL_FAILURE(
      CreateMemory(AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_EXECUTE,
                   kByteLength, &commands));
  ASSERT_NO_FATAL_FAILURE(
      CreateMemory(AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE,
                   kByteLength, &completion));
  ASSERT_EQ(source->device_address % sizeof(uint64_t), 0u);
  ASSERT_EQ(target->device_address % sizeof(uint64_t), 0u);
  ASSERT_EQ(commands->device_address % kCommandByteStride, 0u);
  ASSERT_LT(commands->device_address, kIbAddressLimit);
  ASSERT_LE(commands->info.byte_length,
            kIbAddressLimit - commands->device_address);
  ASSERT_EQ(completion->device_address % alignof(aql::Signal), 0u);

  std::array<uint32_t, kWordCount> expected_commands = {};
  std::array<aql::Packet, kPayloads.size()> packets;
  for (size_t epoch = 0; epoch < kPayloads.size(); ++epoch) {
    const size_t byte_offset = epoch * kCommandByteStride;
    auto* words = expected_commands.data() + byte_offset / sizeof(uint32_t);
    const auto predicate = aql::Gfx9VirtualXcc0(body_word_count);
    std::memcpy(words, predicate.data(), sizeof(predicate));
    size_t word_count = predicate.size();
    word_count += pm4::WriteData(words + word_count,
                                 source->device_address + kPayloadByteOffset,
                                 kPayloads[epoch].data(), payload_word_count);
    word_count += pm4::CopyData(
        words + word_count, source->device_address + kPayloadByteOffset,
        target->device_address + kPayloadByteOffset, width);
    ASSERT_EQ(word_count, ib_word_count);
    packets[epoch] = aql::Gfx9IndirectBuffer(
        aql::HeaderBarrier::kEnabled, commands->device_address + byte_offset,
        ib_word_count, completion->device_address, kScopes);
  }
  // Both programs and their initialized padding stay immutable through every
  // native use. Completion, not predication, bounds the borrowed IB lifetime.
  std::memcpy(commands->host.pointer, expected_commands.data(),
              sizeof(expected_commands));
  std::memset(completion->host.pointer, 0, kByteLength);
  auto& signal = *static_cast<aql::Signal*>(completion->host.pointer);
  signal.kind = 1;
  auto* completion_guard_address =
      static_cast<uint8_t*>(completion->host.pointer) + sizeof(aql::Signal);
  std::array<uint32_t, kCompletionGuardWordCount> completion_words;
  completion_words.fill(kCompletionGuard);
  // Native-owned fields are initialized according to their ABI. Only bytes
  // beyond the complete 64-byte signal block are arbitrary guards.
  std::memcpy(completion_guard_address, completion_words.data(),
              sizeof(completion_words));

  RecordProperty("aql_transfer_width_bits", payload_word_count * 32);
  RecordProperty("aql_transfer_body_word_count", body_word_count);
  RecordProperty("aql_transfer_ib_word_count", ib_word_count);
  RecordProperty("aql_transfer_virtual_xcc_mask", 1);
  RecordProperty("aql_transfer_memory_class", source->info.memory_class);
  RecordProperty("aql_transfer_source_memory_profile_ordinal",
                 source->info.memory_profile_ordinal);
  RecordProperty("aql_transfer_target_memory_profile_ordinal",
                 target->info.memory_profile_ordinal);
  RecordProperty("aql_transfer_ib_memory_profile_ordinal",
                 commands->info.memory_profile_ordinal);
  RecordProperty("aql_transfer_completion_memory_profile_ordinal",
                 completion->info.memory_profile_ordinal);
  RecordProperty("aql_transfer_data_access_flags",
                 std::to_string(source->access_info.flags));
  RecordProperty("aql_transfer_ib_access_flags",
                 std::to_string(commands->access_info.flags));

  GpuUserQueue* queue = nullptr;
  ASSERT_NO_FATAL_FAILURE(CreateQueue(&queue));
  ASSERT_GE(queue->host.ring_byte_length / sizeof(aql::Packet),
            kPayloads.size());
  uint64_t index = 0;
  std::array<uint32_t, kWordCount> expected_source;
  std::array<uint32_t, kWordCount> expected_target;
  std::array<uint32_t, kWordCount> source_words;
  std::array<uint32_t, kWordCount> target_words;
  std::array<uint32_t, kWordCount> command_words;
  for (size_t epoch = 0; epoch < kPayloads.size(); ++epoch) {
    SCOPED_TRACE(epoch);
    expected_source.fill(kSourceGuard);
    expected_target.fill(kTargetGuard);
    source_words = expected_source;
    target_words = expected_target;
    for (uint32_t i = 0; i < payload_word_count; ++i) {
      const uint32_t expected = kPayloads[epoch][i];
      expected_source[kPayloadWordOffset + i] = expected;
      expected_target[kPayloadWordOffset + i] = expected;
      source_words[kPayloadWordOffset + i] = ~expected;
      target_words[kPayloadWordOffset + i] = expected ^ 0xa5a5a5a5u;
    }
    std::memcpy(source->host.pointer, source_words.data(),
                sizeof(source_words));
    std::memcpy(target->host.pointer, target_words.data(),
                sizeof(target_words));
    signal.value = 1;
    GpuStoreRelease(queue->host.write_index_address, index + 1);
    Publish(*queue, index++, packets[epoch]);

    // Native carrier completion ends confirmed CP work and releases its SYSTEM
    // results. Capture every observation before diagnostics or consumption.
    GpuWaitEqual<int64_t>(reinterpret_cast<uintptr_t>(&signal.value), 0);
    std::memcpy(target_words.data(), target->host.pointer,
                sizeof(target_words));
    std::memcpy(source_words.data(), source->host.pointer,
                sizeof(source_words));
    std::memcpy(command_words.data(), commands->host.pointer,
                sizeof(command_words));
    aql::Signal completed_signal = {};
    std::memcpy(&completed_signal, completion->host.pointer,
                sizeof(completed_signal));
    std::memcpy(completion_words.data(), completion_guard_address,
                sizeof(completion_words));
    for (size_t i = 0; i < kWordCount; ++i) {
      EXPECT_EQ(target_words[i], expected_target[i]) << "target word=" << i;
      EXPECT_EQ(source_words[i], expected_source[i]) << "source word=" << i;
      EXPECT_EQ(command_words[i], expected_commands[i]) << "IB word=" << i;
    }
    EXPECT_EQ(completed_signal.kind, 1);
    EXPECT_EQ(completed_signal.value, 0);
    for (size_t i = 0; i < completion_words.size(); ++i) {
      EXPECT_EQ(completion_words[i], kCompletionGuard)
          << "completion guard word=" << i;
    }
    // Even failed nonfatal oracles reach retirement. A failed observation or
    // cleanup stops before rearming the signal or modifying either data page.
    EXPECT_NO_FATAL_FAILURE(queue->WaitConsumed(api_, index));
    if (HasFailure()) {
      return;
    }
  }
  RecordProperty("aql_transfer_completed_epochs", kPayloads.size());
  RecordProperty("aql_transfer_immutable_ib_checks", kPayloads.size());
  RecordProperty("aql_transfer_final_packet_index", std::to_string(index));
}

INSTANTIATE_TEST_SUITE_P(
    Width, AqlTransferTest,
    ::testing::Values(pm4::CopyDataWidth::k32Bit, pm4::CopyDataWidth::k64Bit),
    [](const ::testing::TestParamInfo<pm4::CopyDataWidth>& info) {
      return info.param == pm4::CopyDataWidth::k32Bit ? "Dword" : "Qword";
    });

}  // namespace
