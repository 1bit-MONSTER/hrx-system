// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <array>
#include <cstring>
#include <span>
#include <string>

#include "libamdf/cts/gpu/sdma/encoding/commands.h"
#include "libamdf/cts/gpu/util/command_fixture.h"

namespace {

class SdmaWriteTest : public GpuCommandTest {
 protected:
  SdmaWriteTest()
      : GpuCommandTest({
            .command_type = AMDF_QUEUE_COMMAND_TYPE_GPU_SDMA,
            .roles = AMDF_QUEUE_ROLE_TRANSFER,
            .publication_modes = AMDF_QUEUE_PUBLICATION_MODE_USER |
                                 AMDF_QUEUE_PUBLICATION_MODE_KERNEL,
        }) {}
};

TEST_F(SdmaWriteTest, InlineDataFeedsDependentCopiesAcrossEpochs) {
  constexpr size_t kDataLength = 8192;
  constexpr size_t kControlLength = 4096;
  constexpr size_t kCompletionOffset = 64;
  struct WriteSpan {
    // Byte offset of the inline write within the target allocation.
    uint32_t target_offset;
    // Byte offset of its dependent copy within the output allocation.
    uint32_t output_offset;
    // Nonzero number of inline DWORD values.
    uint32_t word_count;
  };
  constexpr std::array<WriteSpan, 4> kSpans = {
      {{64, 192, 1}, {124, 252, 3}, {4092, 4076, 8}, {6148, 6164, 257}}};
  constexpr std::array<uint32_t, 2> kSeeds = {0x13579bdf, 0xa5c31f27};
  constexpr size_t kInlineWordCount = 1 + 3 + 8 + 257;
  const bool user_gcr =
      (family_.format_features & AMDF_GPU_SDMA_FORMAT_FEATURE_USER_GCR) != 0;
  const size_t words_per_epoch =
      kSpans.size() * (4 + 7) + kInlineWordCount + 1 + 4 + (user_gcr ? 10 : 0);

  GpuMemory* target = nullptr;
  GpuMemory* output = nullptr;
  GpuMemory* completion = nullptr;
  ASSERT_NO_FATAL_FAILURE(
      CreateMemory(AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE,
                   kDataLength, &target));
  ASSERT_NO_FATAL_FAILURE(
      CreateMemory(AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE,
                   kDataLength, &output));
  ASSERT_NO_FATAL_FAILURE(
      CreateMemory(AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE,
                   kControlLength, &completion));

  std::array<uint32_t, kDataLength / sizeof(uint32_t)> expected_target;
  std::array<uint32_t, kDataLength / sizeof(uint32_t)> expected_output;
  std::array<uint32_t, kControlLength / sizeof(uint32_t)> expected_completion;
  std::array<uint32_t, kDataLength / sizeof(uint32_t)> observed_target;
  std::array<uint32_t, kDataLength / sizeof(uint32_t)> observed_output;
  std::array<uint32_t, kControlLength / sizeof(uint32_t)> observed_completion;
  std::array<uint32_t, 257> values;
  auto* target_words = static_cast<uint32_t*>(target->host.pointer);
  auto* output_words = static_cast<uint32_t*>(output->host.pointer);
  auto* completion_words = static_cast<uint32_t*>(completion->host.pointer);
  GpuCommandQueue* queue = nullptr;
  ASSERT_NO_FATAL_FAILURE(CreateQueue(&queue));
  ASSERT_GT(queue->words().size(), kSeeds.size() * words_per_epoch);
  SdmaCommandWriter commands(queue->words().data(), family_.format_features);

  RecordProperty("sdma_format_features",
                 std::to_string(family_.format_features));
  RecordProperty("writes_per_epoch", kSpans.size());
  RecordProperty("inline_words_per_epoch", kInlineWordCount);
  RecordProperty("words_per_epoch", words_per_epoch);
  RecordProperty("target_checked_bytes_per_epoch", kDataLength);
  RecordProperty("output_checked_bytes_per_epoch", kDataLength);
  RecordProperty("control_checked_bytes_per_epoch", kControlLength);
  RecordProperty("completed_epochs", 0);

  for (size_t epoch = 0; epoch < kSeeds.size(); ++epoch) {
    SCOPED_TRACE(epoch);
    const uint32_t seed = kSeeds[epoch];
    const uint32_t marker = static_cast<uint32_t>(epoch + 1);
    for (size_t i = 0; i < expected_target.size(); ++i) {
      const uint32_t word = static_cast<uint32_t>(i);
      expected_target[i] = seed ^ (0x25a64bc3u + word * 0x03050709u);
      expected_output[i] = seed ^ (0x4962d5e7u + word * 0x0507090bu);
    }
    for (size_t i = 0; i < expected_completion.size(); ++i) {
      expected_completion[i] =
          seed ^ (0x6de8912fu + static_cast<uint32_t>(i) * 0x07090b0du);
    }
    std::memcpy(target->host.pointer, expected_target.data(), kDataLength);
    std::memcpy(output->host.pointer, expected_output.data(), kDataLength);
    std::memcpy(completion->host.pointer, expected_completion.data(),
                kControlLength);
    completion_words[kCompletionOffset / sizeof(uint32_t)] = 0;
    expected_completion[kCompletionOffset / sizeof(uint32_t)] = marker;

    if (user_gcr) {
      commands.AcquireFromSystem();
    }
    for (size_t span_index = 0; span_index < kSpans.size(); ++span_index) {
      const auto& span = kSpans[span_index];
      for (uint32_t i = 0; i < span.word_count; ++i) {
        // The CPU formula supplies the oracle independently of device output.
        const uint32_t value =
            seed ^ ((static_cast<uint32_t>(span_index) + 1) * 0x179b3de1u +
                    i * 0x01030507u);
        values[i] = value;
        expected_target[span.target_offset / sizeof(uint32_t) + i] = value;
        expected_output[span.output_offset / sizeof(uint32_t) + i] = value;
        // Every touched word differs from the required result at publication.
        target_words[span.target_offset / sizeof(uint32_t) + i] = ~value;
        output_words[span.output_offset / sizeof(uint32_t) + i] = ~value;
      }
      commands.WriteLinear(target->device_address + span.target_offset,
                           std::span(values).first(span.word_count));
    }
    // Values are now inline; the original CPU array is no longer an input.
    values.fill(0);
    // Writes may overlap. Join all pending writes before their copy consumers;
    // no host wait or intermediate payload observation supplies this edge.
    commands.Noop();
    for (const auto& span : kSpans) {
      commands.CopyLinear(target->device_address + span.target_offset,
                          output->device_address + span.output_offset,
                          span.word_count * sizeof(uint32_t));
    }
    if (user_gcr) {
      commands.ReleaseToSystem();
    }
    commands.Fence32(completion->device_address + kCompletionOffset, marker);
    ASSERT_EQ(commands.word_count(), (epoch + 1) * words_per_epoch);
    ASSERT_NO_FATAL_FAILURE(
        queue->Publish(api_, gpu_api_, commands.word_count()));
    GpuWaitEqual<uint32_t>(
        reinterpret_cast<uintptr_t>(completion->host.pointer) +
            kCompletionOffset,
        marker);

    // Snapshot the final consumer first, before diagnostics or retirement can
    // add synchronization. Check all untouched backing and control guards too.
    std::memcpy(observed_output.data(), output->host.pointer, kDataLength);
    std::memcpy(observed_target.data(), target->host.pointer, kDataLength);
    std::memcpy(observed_completion.data(), completion->host.pointer,
                kControlLength);
    for (size_t i = 0; i < expected_output.size(); ++i) {
      EXPECT_EQ(observed_output[i], expected_output[i]) << "output word " << i;
      EXPECT_EQ(observed_target[i], expected_target[i]) << "target word " << i;
    }
    for (size_t i = 0; i < expected_completion.size(); ++i) {
      EXPECT_EQ(observed_completion[i], expected_completion[i])
          << "control word " << i;
    }
    // Retire even after an oracle failure, but reuse no storage after failure.
    ASSERT_NO_FATAL_FAILURE(queue->WaitRetired(api_));
    if (HasFailure()) {
      return;
    }
    RecordProperty("completed_epochs", epoch + 1);
    RecordProperty("retired_byte_frontier",
                   commands.word_count() * sizeof(uint32_t));
  }
}

}  // namespace
