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
  static constexpr size_t kByteLength = 4096;
  static constexpr size_t kCommandByteStride = 64;

  struct TransferPages {
    // Copy source borrowed from the case-owned memory allocations.
    GpuMemory* source = nullptr;
    // Copy destination borrowed from the case-owned memory allocations.
    GpuMemory* target = nullptr;
    // Executable page kept immutable throughout its native uses.
    GpuMemory* commands = nullptr;
    // Complete native signal block and guards outside its ABI extent.
    GpuMemory* completion = nullptr;
  };

  AqlTransferTest()
      : AqlQueueTest(AMDF_QUEUE_ROLE_TRANSFER | AMDF_QUEUE_ROLE_CACHE_CONTROL) {
  }

  void AllocatePages(amdf_memory_access_t source_access,
                     TransferPages* out_pages) {
    ASSERT_NO_FATAL_FAILURE(
        CreateMemory(source_access, kByteLength, &out_pages->source));
    ASSERT_NO_FATAL_FAILURE(
        CreateMemory(AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE,
                     kByteLength, &out_pages->target));
    ASSERT_NO_FATAL_FAILURE(
        CreateMemory(AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_EXECUTE,
                     kByteLength, &out_pages->commands));
    ASSERT_NO_FATAL_FAILURE(
        CreateMemory(AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE,
                     kByteLength, &out_pages->completion));
    ASSERT_EQ(out_pages->source->device_address % sizeof(uint64_t), 0u);
    ASSERT_EQ(out_pages->target->device_address % sizeof(uint64_t), 0u);
    ASSERT_EQ(out_pages->commands->device_address % kCommandByteStride, 0u);
    constexpr uint64_t kIbAddressLimit = UINT64_C(1) << 48;
    ASSERT_LT(out_pages->commands->device_address, kIbAddressLimit);
    ASSERT_LE(out_pages->commands->info.byte_length,
              kIbAddressLimit - out_pages->commands->device_address);
    ASSERT_EQ(out_pages->completion->device_address % alignof(aql::Signal), 0u);
  }
};

TEST_P(AqlTransferTest, ConfirmedWriteFeedsCopyAcrossEpochs) {
  if ((features_ & AMDF_GPU_DEVICE_FEATURE_LOCAL_MEMORY) == 0) {
    GTEST_SKIP() << "AQL transfers require the discrete coherent SYSTEM memory "
                    "policy";
  }
  constexpr size_t kWordCount = kByteLength / sizeof(uint32_t);
  constexpr size_t kPayloadByteOffset = 64;
  constexpr size_t kPayloadWordOffset = kPayloadByteOffset / sizeof(uint32_t);
  constexpr size_t kCompletionGuardWordCount =
      (kByteLength - sizeof(aql::Signal)) / sizeof(uint32_t);
  constexpr uint32_t kSourceGuard = 0x759bf13du;
  constexpr uint32_t kTargetGuard = 0x4e90b725u;
  constexpr uint32_t kCompletionGuard = 0x68d329b7u;
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

  TransferPages pages;
  ASSERT_NO_FATAL_FAILURE(AllocatePages(
      AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE, &pages));
  GpuMemory* source = pages.source;
  GpuMemory* target = pages.target;
  GpuMemory* commands = pages.commands;
  GpuMemory* completion = pages.completion;

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

TEST_P(AqlTransferTest, CpuPublishedSourceFeedsCopyAcrossEpochs) {
  if ((features_ & AMDF_GPU_DEVICE_FEATURE_LOCAL_MEMORY) == 0) {
    GTEST_SKIP() << "AQL transfers require the discrete coherent SYSTEM memory "
                    "policy";
  }
  constexpr size_t kWordCount = kByteLength / sizeof(uint32_t);
  constexpr size_t kPayloadByteOffset = 64;
  constexpr size_t kPayloadWordOffset = kPayloadByteOffset / sizeof(uint32_t);
  constexpr size_t kCompletionGuardWordCount =
      (kByteLength - sizeof(aql::Signal)) / sizeof(uint32_t);
  constexpr uint32_t kSourceGuard = 0x759bf13du;
  constexpr uint32_t kTargetGuard = 0x4e90b725u;
  constexpr uint32_t kCompletionGuard = 0x68d329b7u;
  constexpr std::array<std::array<uint32_t, 2>, 2> kPayloads = {
      {{0x13579bdfu, 0x2468ace0u}, {0xfdb97531u, 0x80a6c42eu}}};
  constexpr uint32_t kBodyWordCount = 6;
  constexpr uint32_t kIbWordCount = 2 + kBodyWordCount;
  const pm4::CopyDataWidth width = GetParam();
  const uint32_t payload_word_count =
      width == pm4::CopyDataWidth::k32Bit ? 1 : 2;

  TransferPages pages;
  ASSERT_NO_FATAL_FAILURE(AllocatePages(AMDF_MEMORY_ACCESS_READ, &pages));
  GpuMemory* source = pages.source;
  GpuMemory* target = pages.target;
  GpuMemory* commands = pages.commands;
  GpuMemory* completion = pages.completion;
  constexpr amdf_memory_map_flags_t kHostAccess =
      AMDF_MEMORY_MAP_FLAG_READ | AMDF_MEMORY_MAP_FLAG_WRITE;
  ASSERT_EQ(source->host.flags & kHostAccess, kHostAccess);
  ASSERT_EQ(target->host.flags & kHostAccess, kHostAccess);

  // Each query describes two sites on one backing. Family transfer admission
  // separately permits the COPY opcode; a pair answer only supplies visibility.
  const amdf_memory_site_t source_host = source->HostSite();
  const amdf_memory_site_t source_device = source->DeviceSite(family_.ordinal);
  const amdf_memory_site_t target_device = target->DeviceSite(family_.ordinal);
  const amdf_memory_site_t target_host = target->HostSite();
  amdf_memory_pair_info_t ingress = {};
  ingress.type = AMDF_STRUCTURE_TYPE_MEMORY_PAIR_INFO;
  ingress.structure_size = sizeof(ingress);
  ASSERT_EQ(
      api_->memory_query_pair_info(&source_host, &source_device, &ingress),
      AMDF_STATUS_OK);
  ASSERT_NE(ingress.flags & AMDF_MEMORY_PAIR_FLAG_SHARED_BACKING_REACHABLE, 0u);
  amdf_memory_pair_info_t egress = {};
  egress.type = AMDF_STRUCTURE_TYPE_MEMORY_PAIR_INFO;
  egress.structure_size = sizeof(egress);
  ASSERT_EQ(api_->memory_query_pair_info(&target_device, &target_host, &egress),
            AMDF_STATUS_OK);
  ASSERT_NE(egress.flags & AMDF_MEMORY_PAIR_FLAG_SHARED_BACKING_REACHABLE, 0u);

  const auto check_transition = [](const amdf_cache_transition_t& transition,
                                   amdf_cache_transition_kind_t kind,
                                   amdf_cache_transition_executor_t executor,
                                   amdf_cache_operation_t operation) {
    ASSERT_EQ(transition.kind, kind);
    ASSERT_EQ(transition.executor, executor);
    ASSERT_EQ(transition.operation, operation);
    ASSERT_EQ(transition.host_operation, AMDF_HOST_CACHE_OPERATION_NONE);
    ASSERT_EQ(transition.host_instruction, AMDF_HOST_CACHE_INSTRUCTION_NONE);
    ASSERT_EQ(transition.host_fence_before, AMDF_HOST_CACHE_FENCE_NONE);
    ASSERT_EQ(transition.host_fence_after, AMDF_HOST_CACHE_FENCE_NONE);
    ASSERT_EQ(transition.range_granularity, 0u);
  };
  ASSERT_NO_FATAL_FAILURE(check_transition(
      ingress.release, AMDF_CACHE_TRANSITION_KIND_NONE,
      AMDF_CACHE_TRANSITION_EXECUTOR_NONE, AMDF_CACHE_OPERATION_NONE));
  aql::FenceScopes scopes = {aql::FenceScope::kNone, aql::FenceScope::kNone};
  ASSERT_NO_FATAL_FAILURE(
      check_transition(ingress.acquire, AMDF_CACHE_TRANSITION_KIND_GLOBAL,
                       AMDF_CACHE_TRANSITION_EXECUTOR_QUEUE,
                       AMDF_CACHE_OPERATION_ACQUIRE_FROM_SYSTEM));
  // The admitted AQL carrier realizes these exact global queue operations with
  // SYSTEM scopes. Neither coherent host side requires cache maintenance.
  scopes.acquire = aql::FenceScope::kSystem;
  ASSERT_NO_FATAL_FAILURE(
      check_transition(egress.release, AMDF_CACHE_TRANSITION_KIND_GLOBAL,
                       AMDF_CACHE_TRANSITION_EXECUTOR_QUEUE,
                       AMDF_CACHE_OPERATION_RELEASE_TO_SYSTEM));
  scopes.release = aql::FenceScope::kSystem;
  ASSERT_NO_FATAL_FAILURE(check_transition(
      egress.acquire, AMDF_CACHE_TRANSITION_KIND_NONE,
      AMDF_CACHE_TRANSITION_EXECUTOR_NONE, AMDF_CACHE_OPERATION_NONE));

  std::array<uint32_t, kWordCount> expected_commands = {};
  const auto predicate = aql::Gfx9VirtualXcc0(kBodyWordCount);
  std::memcpy(expected_commands.data(), predicate.data(), sizeof(predicate));
  size_t word_count = predicate.size();
  word_count +=
      pm4::CopyData(expected_commands.data() + word_count,
                    source->device_address + kPayloadByteOffset,
                    target->device_address + kPayloadByteOffset, width);
  ASSERT_EQ(word_count, kIbWordCount);
  const aql::Packet packet = aql::Gfx9IndirectBuffer(
      aql::HeaderBarrier::kEnabled, commands->device_address, kIbWordCount,
      completion->device_address, scopes);
  // One program and its initialized padding remain unchanged across both
  // epochs. Only CPU stores produce the data read by COPY_DATA.
  std::memcpy(commands->host.pointer, expected_commands.data(),
              sizeof(expected_commands));
  std::memset(completion->host.pointer, 0, kByteLength);
  auto& signal = *static_cast<aql::Signal*>(completion->host.pointer);
  signal.kind = 1;
  auto* completion_guard_address =
      static_cast<uint8_t*>(completion->host.pointer) + sizeof(aql::Signal);
  std::array<uint32_t, kCompletionGuardWordCount> completion_words;
  completion_words.fill(kCompletionGuard);
  // The complete native signal is zero-initialized, with guards only outside
  // its 64-byte ABI extent. Native-owned fields are not repurposed as canaries.
  std::memcpy(completion_guard_address, completion_words.data(),
              sizeof(completion_words));

  const auto describe_transition =
      [](const amdf_cache_transition_t& transition) {
        return "kind=" + std::to_string(transition.kind) +
               ",executor=" + std::to_string(transition.executor) +
               ",operation=" + std::to_string(transition.operation);
      };
  RecordProperty("aql_transfer_ingress_flags", std::to_string(ingress.flags));
  RecordProperty("aql_transfer_ingress_release",
                 describe_transition(ingress.release));
  RecordProperty("aql_transfer_ingress_acquire",
                 describe_transition(ingress.acquire));
  RecordProperty("aql_transfer_egress_flags", std::to_string(egress.flags));
  RecordProperty("aql_transfer_egress_release",
                 describe_transition(egress.release));
  RecordProperty("aql_transfer_egress_acquire",
                 describe_transition(egress.acquire));
  RecordProperty("aql_transfer_acquire_scope",
                 static_cast<uint32_t>(scopes.acquire));
  RecordProperty("aql_transfer_release_scope",
                 static_cast<uint32_t>(scopes.release));
  RecordProperty("aql_transfer_width_bits", payload_word_count * 32);
  RecordProperty("aql_transfer_body_word_count", kBodyWordCount);
  RecordProperty("aql_transfer_ib_word_count", kIbWordCount);
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
  RecordProperty("aql_transfer_source_device_access",
                 source->access_info.access);
  RecordProperty("aql_transfer_target_device_access",
                 target->access_info.access);
  RecordProperty("aql_transfer_ib_device_access", commands->access_info.access);
  RecordProperty("aql_transfer_completion_device_access",
                 completion->access_info.access);
  RecordProperty("aql_transfer_source_host_cacheability",
                 source->host.cacheability);
  RecordProperty("aql_transfer_target_host_cacheability",
                 target->host.cacheability);
  RecordProperty("aql_transfer_source_host_access", source->host.flags);
  RecordProperty("aql_transfer_target_host_access", target->host.flags);

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
    target_words = expected_target;
    for (uint32_t i = 0; i < payload_word_count; ++i) {
      const uint32_t expected = kPayloads[epoch][i];
      expected_source[kPayloadWordOffset + i] = expected;
      expected_target[kPayloadWordOffset + i] = expected;
      target_words[kPayloadWordOffset + i] = ~expected;
    }
    std::memcpy(source->host.pointer, expected_source.data(),
                sizeof(expected_source));
    std::memcpy(target->host.pointer, target_words.data(),
                sizeof(target_words));
    signal.value = 1;
    GpuStoreRelease(queue->host.write_index_address, index + 1);
    Publish(*queue, index++, packet);

    // Acquire the native completion and capture all memory before diagnostics
    // or retirement can contribute any other observation boundary.
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
    // Even failed nonfatal oracles reach AQL slot retirement. Completion and
    // consumption both precede CPU updates or signal rearm.
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
