// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <tuple>
#include <vector>

#include "libamdf/cts/gpu/pm4/command_fixture.h"
#include "libamdf/cts/gpu/pm4/encoding/commands.h"

namespace {

constexpr uint32_t kDataByteLength = 65536;
constexpr uint32_t kControlByteLength = 4096;
constexpr uint32_t kCompletionWord = 64;
constexpr uint32_t kEpochCount = 2;

struct DmaCopyCase {
  // Stable name for the byte-count and alignment partition.
  const char* name;
  // Source byte offset, with readable trailing backing retained.
  uint32_t source_offset;
  // First destination and next source in the two-copy composition.
  uint32_t intermediate_offset;
  // Byte offset of the final destination in either composition.
  uint32_t target_offset;
  // Positive direct byte count, at most RADV's GFX11+ chunk policy.
  uint32_t byte_length;
};

constexpr DmaCopyCase kCases[] = {
    {"Byte", 65, 131, 197, 1},
    {"ThreeBytes", 67, 133, 199, 3},
    {"UnalignedDword", 69, 135, 201, 4},
    {"SevenBytes", 71, 137, 203, 7},
    {"UnalignedQword", 73, 139, 205, 8},
    {"Before32Bytes", 95, 159, 223, 31},
    {"Bytes32", 96, 160, 224, 32},
    {"After32Bytes", 97, 161, 225, 33},
    {"OneKiB", 64, 192, 256, 1024},
    {"SourcePage", 4093, 131, 197, 1027},
    {"IntermediatePage", 65, 4095, 197, 1025},
    {"TargetPage", 65, 131, 4095, 1027},
    {"BothPages", 4093, 4091, 4095, 4097},
    {"DriverChunk", 65, 131, 197, 32736},
    {"FinalDestinationByte", 65, 4093, kDataByteLength - 17, 17},
};

// The complete table has owned operands before any native case can execute.
static_assert([] {
  for (const auto& test : kCases) {
    if (test.byte_length == 0 || test.byte_length > 32736 ||
        test.source_offset + test.byte_length + 64 > kDataByteLength ||
        test.intermediate_offset + test.byte_length + 64 > kDataByteLength ||
        test.target_offset + test.byte_length > kDataByteLength) {
      return false;
    }
  }
  return true;
}());

enum class DmaCopyPath {
  kDirect,
  kDependent,
};

class Pm4DmaTest : public Pm4CommandTest,
                   public ::testing::WithParamInterface<DmaCopyCase> {
 protected:
  void RunCopy(DmaCopyPath path) {
    const auto& test = GetParam();
    const bool dependent = path == DmaCopyPath::kDependent;
    GpuMemory* source = nullptr;
    GpuMemory* intermediate = nullptr;
    GpuMemory* target = nullptr;
    GpuMemory* completion = nullptr;
    constexpr auto kReadWrite =
        AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE;
    ASSERT_NO_FATAL_FAILURE(
        CreateMemory(AMDF_MEMORY_ACCESS_READ, kDataByteLength, &source));
    if (dependent) {
      ASSERT_NO_FATAL_FAILURE(
          CreateMemory(kReadWrite, kDataByteLength, &intermediate));
    }
    ASSERT_NO_FATAL_FAILURE(CreateMemory(kReadWrite, kDataByteLength, &target));
    ASSERT_NO_FATAL_FAILURE(
        CreateMemory(kReadWrite, kControlByteLength, &completion));

    GpuCommandQueue* queue = nullptr;
    ASSERT_NO_FATAL_FAILURE(CreateQueue(&queue));
    ASSERT_GT(queue->words().size(), kEpochCount * 48u);
    std::vector<uint32_t> expected_commands(queue->words().size(), 0x6935bdefu);
    std::vector<uint32_t> observed_commands(queue->words().size());
    std::memcpy(queue->words().data(), expected_commands.data(),
                queue->words().size_bytes());
    std::array<uint64_t, kEpochCount> frontiers;
    Pm4CommandWriter commands(queue->words().data(), *pm4_profile_);
    for (uint32_t epoch = 0; epoch < kEpochCount; ++epoch) {
      commands.SystemBarrier();
      if (dependent) {
        commands.DmaCopy(
            source->device_address + test.source_offset,
            intermediate->device_address + test.intermediate_offset,
            test.byte_length);
        // RAW_WAIT on the next DMA supplies the read-after-write dependency.
        // No intervening host observation, drain, or cache command joins it.
        commands.DmaCopy(
            intermediate->device_address + test.intermediate_offset,
            target->device_address + test.target_offset, test.byte_length);
      } else {
        commands.DmaCopy(source->device_address + test.source_offset,
                         target->device_address + test.target_offset,
                         test.byte_length);
      }
      commands.WaitDma();
      commands.SystemBarrier();
      commands.WriteData32(
          completion->device_address + kCompletionWord * sizeof(uint32_t),
          epoch + 1);
      commands.PadToEightWords();
      frontiers[epoch] = commands.word_count();
      ASSERT_EQ(frontiers[epoch], (epoch + 1) * 48u);
    }
    std::memcpy(expected_commands.data(), queue->words().data(),
                queue->words().size_bytes());

    // Heap-backed snapshots keep the same test usable with native Windows
    // stack limits. All storage exists before the first queue publication.
    std::vector<uint8_t> expected_source(kDataByteLength);
    std::vector<uint8_t> expected_target(kDataByteLength);
    std::vector<uint8_t> observed_source(kDataByteLength);
    std::vector<uint8_t> observed_target(kDataByteLength);
    std::vector<uint8_t> expected_intermediate(dependent ? kDataByteLength : 0);
    std::vector<uint8_t> observed_intermediate(dependent ? kDataByteLength : 0);
    std::array<uint32_t, kControlByteLength / sizeof(uint32_t)>
        expected_control;
    std::array<uint32_t, kControlByteLength / sizeof(uint32_t)>
        observed_control;
    for (uint32_t i = 0; i < expected_control.size(); ++i) {
      expected_control[i] = 0x68d329b7u ^ (i * 0x01030507u);
    }
    expected_control[kCompletionWord] = 0;
    std::memcpy(completion->host.pointer, expected_control.data(),
                sizeof(expected_control));
    RecordProperty("pm4_dma_copy_byte_length", test.byte_length);
    RecordProperty("pm4_dma_source_byte_offset", test.source_offset);
    RecordProperty("pm4_dma_target_byte_offset", test.target_offset);
    RecordProperty("pm4_dma_copies_per_epoch", dependent ? 2 : 1);
    RecordProperty("pm4_dma_checked_data_bytes_per_epoch",
                   (dependent ? 3 : 2) * kDataByteLength);
    RecordProperty("pm4_dma_checked_control_bytes_per_epoch",
                   kControlByteLength);
    RecordProperty("pm4_dma_checked_command_bytes_per_epoch",
                   queue->words().size_bytes());
    RecordProperty("pm4_dma_completed_epochs", 0);
    if (dependent) {
      RecordProperty("pm4_dma_intermediate_byte_offset",
                     test.intermediate_offset);
    }

    for (uint32_t epoch = 1; epoch <= kEpochCount; ++epoch) {
      SCOPED_TRACE(epoch);
      for (uint32_t i = 0; i < kDataByteLength; ++i) {
        expected_source[i] = static_cast<uint8_t>(
            (i * 37u) ^ (i >> 3) ^ (i >> 11) ^ (epoch * 83u) ^ 0x5au);
        expected_target[i] = static_cast<uint8_t>((i * 29u) ^ (i >> 7) ^ 0x96u);
        if (dependent) {
          expected_intermediate[i] =
              static_cast<uint8_t>((i * 43u) ^ (i >> 5) ^ 0xacu);
        }
      }
      observed_target = expected_target;
      observed_intermediate = expected_intermediate;
      for (uint32_t i = 0; i < test.byte_length; ++i) {
        const uint8_t value = expected_source[test.source_offset + i];
        expected_target[test.target_offset + i] = value;
        observed_target[test.target_offset + i] =
            static_cast<uint8_t>(value ^ 0xffu);
        if (dependent) {
          expected_intermediate[test.intermediate_offset + i] = value;
          observed_intermediate[test.intermediate_offset + i] =
              static_cast<uint8_t>(value ^ 0xffu);
        }
      }
      std::memcpy(source->host.pointer, expected_source.data(),
                  kDataByteLength);
      std::memcpy(target->host.pointer, observed_target.data(),
                  kDataByteLength);
      if (dependent) {
        std::memcpy(intermediate->host.pointer, observed_intermediate.data(),
                    kDataByteLength);
      }
      expected_control[kCompletionWord] = epoch;
      ASSERT_NO_FATAL_FAILURE(
          queue->Publish(api_, gpu_api_, frontiers[epoch - 1]));
      ASSERT_TRUE(GpuWaitEqual<uint32_t>(
          reinterpret_cast<uintptr_t>(completion->host.pointer) +
              kCompletionWord * sizeof(uint32_t),
          epoch));

      // Capture the final consumer first, then all backing, before diagnostics
      // or native retirement can add any other synchronization.
      std::memcpy(observed_target.data(), target->host.pointer,
                  kDataByteLength);
      std::memcpy(observed_source.data(), source->host.pointer,
                  kDataByteLength);
      if (dependent) {
        std::memcpy(observed_intermediate.data(), intermediate->host.pointer,
                    kDataByteLength);
      }
      std::memcpy(observed_control.data(), completion->host.pointer,
                  sizeof(observed_control));
      std::memcpy(observed_commands.data(), queue->words().data(),
                  queue->words().size_bytes());
      EXPECT_EQ(observed_target, expected_target);
      EXPECT_EQ(observed_source, expected_source);
      EXPECT_EQ(observed_intermediate, expected_intermediate);
      EXPECT_EQ(observed_control, expected_control);
      EXPECT_EQ(observed_commands, expected_commands);
      EXPECT_NO_FATAL_FAILURE(queue->WaitRetired(api_));
      if (HasFailure()) {
        return;
      }
      RecordProperty("pm4_dma_completed_epochs", epoch);
    }
  }
};

TEST_P(Pm4DmaTest, CopiesExactBytesAcrossEpochs) {
  RunCopy(DmaCopyPath::kDirect);
}

TEST_P(Pm4DmaTest, RawWaitFeedsDependentCopyAcrossEpochs) {
  RunCopy(DmaCopyPath::kDependent);
}

INSTANTIATE_TEST_SUITE_P(Extent, Pm4DmaTest, ::testing::ValuesIn(kCases),
                         [](const ::testing::TestParamInfo<DmaCopyCase>& info) {
                           return info.param.name;
                         });

struct DmaFillSpan {
  // Stable name for the fill/copy extent and alignment partition.
  const char* name;
  // DWORD-aligned fill destination, then the dependent copy's source.
  uint32_t fill_offset;
  // Byte-aligned destination of the dependent copy.
  uint32_t output_offset;
  // Positive DWORD-aligned byte count within the driver chunk policy.
  uint32_t byte_length;
};

constexpr DmaFillSpan kFillSpans[] = {
    {"Dword", 68, 197, 4},
    {"Qword", 124, 251, 8},
    {"Before32Bytes", 92, 221, 28},
    {"Bytes32", 96, 225, 32},
    {"After32Bytes", 100, 229, 36},
    {"OneKiB", 64, 192, 1024},
    {"FillPage", 4092, 197, 1028},
    {"OutputPage", 64, 4093, 1028},
    {"BothPages", 4092, 4091, 4100},
    {"DriverChunk", 68, 197, 32736},
    {"FinalOutputDword", 64, kDataByteLength - 4, 4},
};

struct DmaFillPatterns {
  // Stable name for the two-epoch pattern partition.
  const char* name;
  // Complete immediate DWORD values, changed only after completed use.
  std::array<uint32_t, kEpochCount> values;
};

constexpr DmaFillPatterns kFillPatterns[] = {
    {"ZeroToOnes", {0, 0xffffffff}},
    {"HighBitToLowBit", {0x80000000, 1}},
    {"DistinctBytes", {0x6d2ac491, 0xb730e85a}},
};

static_assert([] {
  for (const auto& span : kFillSpans) {
    if (span.fill_offset % 4 != 0 || span.byte_length == 0 ||
        span.byte_length % 4 != 0 || span.byte_length > 32736 ||
        span.fill_offset + span.byte_length + 64 > kDataByteLength ||
        span.output_offset + span.byte_length > kDataByteLength) {
      return false;
    }
  }
  return true;
}());

class Pm4DmaFillTest : public Pm4CommandTest,
                       public ::testing::WithParamInterface<
                           std::tuple<DmaFillSpan, DmaFillPatterns>> {};

TEST_P(Pm4DmaFillTest, ImmediatePatternFeedsDependentCopyAcrossEpochs) {
  const auto& [span, patterns] = GetParam();
  constexpr auto kReadWrite =
      AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE;
  GpuMemory* filled = nullptr;
  GpuMemory* output = nullptr;
  GpuMemory* completion = nullptr;
  ASSERT_NO_FATAL_FAILURE(CreateMemory(kReadWrite, kDataByteLength, &filled));
  ASSERT_NO_FATAL_FAILURE(CreateMemory(kReadWrite, kDataByteLength, &output));
  ASSERT_NO_FATAL_FAILURE(
      CreateMemory(kReadWrite, kControlByteLength, &completion));

  GpuCommandQueue* queue = nullptr;
  ASSERT_NO_FATAL_FAILURE(CreateQueue(&queue));
  constexpr uint32_t kWordsPerEpoch = 48;
  ASSERT_GT(queue->words().size(), kEpochCount * kWordsPerEpoch);
  std::vector<uint32_t> expected_commands(queue->words().size(), 0x6935bdefu);
  std::vector<uint32_t> observed_commands(queue->words().size());
  std::memcpy(queue->words().data(), expected_commands.data(),
              queue->words().size_bytes());
  Pm4CommandWriter commands(queue->words().data(), *pm4_profile_);
  for (uint32_t epoch = 0; epoch < kEpochCount; ++epoch) {
    commands.SystemBarrier();
    commands.DmaFill32(filled->device_address + span.fill_offset,
                       patterns.values[epoch], span.byte_length);
    // The consumer's RAW_WAIT joins the fill without an intervening host,
    // cache operation, or DMA drain. Completion joins the whole sequence.
    commands.DmaCopy(filled->device_address + span.fill_offset,
                     output->device_address + span.output_offset,
                     span.byte_length);
    commands.WaitDma();
    commands.SystemBarrier();
    commands.WriteData32(
        completion->device_address + kCompletionWord * sizeof(uint32_t),
        epoch + 1);
    commands.PadToEightWords();
    ASSERT_EQ(commands.word_count(), (epoch + 1) * kWordsPerEpoch);
  }
  std::memcpy(expected_commands.data(), queue->words().data(),
              queue->words().size_bytes());

  std::vector<uint8_t> expected_filled(kDataByteLength);
  std::vector<uint8_t> expected_output(kDataByteLength);
  std::vector<uint8_t> observed_filled(kDataByteLength);
  std::vector<uint8_t> observed_output(kDataByteLength);
  std::array<uint32_t, kControlByteLength / sizeof(uint32_t)> expected_control;
  std::array<uint32_t, kControlByteLength / sizeof(uint32_t)> observed_control;
  for (uint32_t i = 0; i < expected_control.size(); ++i) {
    expected_control[i] = 0x68d329b7u ^ (i * 0x01030507u);
  }
  expected_control[kCompletionWord] = 0;
  std::memcpy(completion->host.pointer, expected_control.data(),
              sizeof(expected_control));
  RecordProperty("pm4_dma_fill_byte_length", span.byte_length);
  RecordProperty("pm4_dma_fill_byte_offset", span.fill_offset);
  RecordProperty("pm4_dma_fill_output_byte_offset", span.output_offset);
  RecordProperty("pm4_dma_fill_pattern_1", std::to_string(patterns.values[0]));
  RecordProperty("pm4_dma_fill_pattern_2", std::to_string(patterns.values[1]));
  RecordProperty("pm4_dma_fill_checked_data_bytes_per_epoch",
                 2 * kDataByteLength);
  RecordProperty("pm4_dma_fill_checked_control_bytes_per_epoch",
                 kControlByteLength);
  RecordProperty("pm4_dma_fill_checked_command_bytes_per_epoch",
                 queue->words().size_bytes());
  RecordProperty("pm4_dma_fill_completed_epochs", 0);

  for (uint32_t epoch = 0; epoch < kEpochCount; ++epoch) {
    SCOPED_TRACE(epoch);
    for (uint32_t i = 0; i < kDataByteLength; ++i) {
      expected_filled[i] =
          static_cast<uint8_t>((i * 43u) ^ (i >> 5) ^ (epoch * 31u) ^ 0xacu);
      expected_output[i] =
          static_cast<uint8_t>((i * 29u) ^ (i >> 7) ^ (epoch * 47u) ^ 0x96u);
    }
    observed_filled = expected_filled;
    observed_output = expected_output;
    for (uint32_t i = 0; i < span.byte_length; ++i) {
      // Model the repeated little-endian DWORD directly, independently of
      // either native buffer. Every payload byte starts different.
      const auto value = static_cast<uint8_t>(patterns.values[epoch] >>
                                              ((i % sizeof(uint32_t)) * 8));
      expected_filled[span.fill_offset + i] = value;
      expected_output[span.output_offset + i] = value;
      observed_filled[span.fill_offset + i] =
          static_cast<uint8_t>(value ^ 0xffu);
      observed_output[span.output_offset + i] =
          static_cast<uint8_t>(value ^ 0xffu);
    }
    std::memcpy(filled->host.pointer, observed_filled.data(), kDataByteLength);
    std::memcpy(output->host.pointer, observed_output.data(), kDataByteLength);
    expected_control[kCompletionWord] = epoch + 1;
    ASSERT_NO_FATAL_FAILURE(
        queue->Publish(api_, gpu_api_, (epoch + 1) * kWordsPerEpoch));
    ASSERT_TRUE(GpuWaitEqual<uint32_t>(
        reinterpret_cast<uintptr_t>(completion->host.pointer) +
            kCompletionWord * sizeof(uint32_t),
        epoch + 1));

    // Capture the final consumer before any other observation or retirement
    // could supply an extra synchronization edge.
    std::memcpy(observed_output.data(), output->host.pointer, kDataByteLength);
    std::memcpy(observed_filled.data(), filled->host.pointer, kDataByteLength);
    std::memcpy(observed_control.data(), completion->host.pointer,
                sizeof(observed_control));
    std::memcpy(observed_commands.data(), queue->words().data(),
                queue->words().size_bytes());
    EXPECT_EQ(observed_output, expected_output);
    EXPECT_EQ(observed_filled, expected_filled);
    EXPECT_EQ(observed_control, expected_control);
    EXPECT_EQ(observed_commands, expected_commands);
    EXPECT_NO_FATAL_FAILURE(queue->WaitRetired(api_));
    if (HasFailure()) {
      return;
    }
    RecordProperty("pm4_dma_fill_completed_epochs", epoch + 1);
  }
}

INSTANTIATE_TEST_SUITE_P(ExtentAndPattern, Pm4DmaFillTest,
                         ::testing::Combine(::testing::ValuesIn(kFillSpans),
                                            ::testing::ValuesIn(kFillPatterns)),
                         [](const auto& info) {
                           return std::string(std::get<0>(info.param).name) +
                                  "_" + std::get<1>(info.param).name;
                         });

}  // namespace
