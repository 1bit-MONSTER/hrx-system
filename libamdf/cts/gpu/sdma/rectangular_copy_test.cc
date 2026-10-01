// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <algorithm>
#include <array>
#include <cstring>
#include <span>
#include <string>
#include <vector>

#include "libamdf/cts/gpu/sdma/encoding/commands.h"
#include "libamdf/cts/gpu/util/command_fixture.h"

namespace {

struct Region {
  // DWORD-aligned packet base within the owned allocation, in bytes.
  uint32_t byte_offset;
  // Coordinates and pitches relative to that packet base.
  SdmaLinearLayout layout;
};

struct Rectangle {
  // Stable diagnostic name for this geometry.
  const char* name;
  // Independent source layout within its owned backing.
  Region source;
  // Independent destination layout within its owned backing.
  Region target;
  // Positive logical dimensions of the transfer.
  SdmaCopyExtent extent;
};

std::array<Rectangle, 3> CommonRectangles(uint32_t element_bytes) {
  const uint32_t page_displacement = std::max(4u, element_bytes);
  return {{
      {"single", {64, {3, 1, 1, 8, 32}}, {192, {5, 2, 2, 12, 48}}, {1, 1, 1}},
      {"volume",
       {128, {3, 2, 1, 64, 512}},
       {256, {5, 1, 2, 96, 960}},
       {17, 3, 2}},
      {"page_crossing",
       {4096 - page_displacement, {0, 0, 0, 64, 256}},
       {8192 - page_displacement, {0, 0, 0, 96, 576}},
       {33, 2, 2}},
  }};
}

// CPU oracle uses byte-address arithmetic, independently of packet placement.
uint64_t RowByteOffset(const Region& region, uint32_t element_bytes,
                       uint32_t row, uint32_t slice) {
  const auto& layout = region.layout;
  return region.byte_offset +
         uint64_t{element_bytes} *
             (layout.x + uint64_t{layout.y + row} * layout.row_pitch +
              uint64_t{layout.z + slice} * layout.slice_pitch);
}

uint8_t Pattern(uint32_t seed, size_t byte_offset) {
  return static_cast<uint8_t>(seed + byte_offset * 73 +
                              (byte_offset >> 8) * 19 +
                              (byte_offset >> 12) * 37);
}

void CheckBytes(const char* label, const std::vector<uint8_t>& actual,
                const std::vector<uint8_t>& expected) {
  size_t first_mismatch = expected.size();
  size_t mismatch_count = 0;
  for (size_t i = 0; i < expected.size(); ++i) {
    if (actual[i] != expected[i]) {
      first_mismatch = std::min(first_mismatch, i);
      ++mismatch_count;
    }
  }
  EXPECT_EQ(mismatch_count, 0u)
      << label << " first differing byte=" << first_mismatch;
}

class SdmaRectangularCopyTest : public GpuCommandTest {
 protected:
  SdmaRectangularCopyTest()
      : GpuCommandTest({
            .command_type = AMDF_QUEUE_COMMAND_TYPE_GPU_SDMA,
            .roles = AMDF_QUEUE_ROLE_TRANSFER,
            .format_features = AMDF_GPU_SDMA_FORMAT_FEATURE_COPY_LINEAR_RECT,
            .publication_modes = AMDF_QUEUE_PUBLICATION_MODE_USER |
                                 AMDF_QUEUE_PUBLICATION_MODE_KERNEL,
        }) {}

  uint32_t ZBitCount() const {
    if ((family_.format_features &
         AMDF_GPU_SDMA_FORMAT_FEATURE_COPY_LINEAR_RECT_WIDE) != 0) {
      return 14;
    }
    return (family_.format_features &
            AMDF_GPU_SDMA_FORMAT_FEATURE_COPY_LINEAR_RECT_EXTENDED_Z) != 0
               ? 13
               : 11;
  }

  void CheckOwnedRegion(const Region& region, const SdmaCopyExtent& extent,
                        uint32_t element_bytes, uint64_t allocation_bytes) {
    const bool wide = (family_.format_features &
                       AMDF_GPU_SDMA_FORMAT_FEATURE_COPY_LINEAR_RECT_WIDE) != 0;
    const auto& layout = region.layout;
    ASSERT_EQ(region.byte_offset % 4, 0u);
    ASSERT_GT(layout.row_pitch, 0u);
    ASSERT_GT(layout.slice_pitch, 0u);
    ASSERT_EQ((layout.row_pitch * element_bytes) % 4, 0u);
    ASSERT_EQ((layout.slice_pitch * element_bytes) % 4, 0u);
    ASSERT_GT(extent.width, 0u);
    ASSERT_GT(extent.height, 0u);
    ASSERT_GT(extent.depth, 0u);
    ASSERT_LE(layout.x + extent.width, layout.row_pitch);
    ASSERT_LE(uint64_t{layout.y + extent.height} * layout.row_pitch,
              layout.slice_pitch);
    ASSERT_LT(layout.x, 1u << (wide ? 16 : 14));
    ASSERT_LT(layout.y, 1u << (wide ? 16 : 14));
    ASSERT_LT(layout.z, 1u << ZBitCount());
    ASSERT_LE(layout.row_pitch, 1u << (wide ? 16 : 19));
    ASSERT_LE(layout.slice_pitch, UINT64_C(1) << (wide ? 32 : 28));
    ASSERT_LE(extent.width, 1u << (wide ? 16 : 14));
    ASSERT_LE(extent.height, 1u << (wide ? 16 : 14));
    ASSERT_LE(extent.depth, 1u << ZBitCount());
    ASSERT_LE(RowByteOffset(region, element_bytes, extent.height - 1,
                            extent.depth - 1) +
                  uint64_t{element_bytes} * extent.width,
              allocation_bytes);
  }

  void Run(uint32_t element_log2, std::span<const Rectangle> rectangles) {
    constexpr size_t kAllocationBytes = 65536;
    constexpr size_t kControlBytes = 4096;
    constexpr size_t kCompletionOffset = 64;
    constexpr std::array<uint32_t, 2> kSeeds = {0x13579bdfu, 0xa5c31f26u};
    const uint32_t element_bytes = 1u << element_log2;
    const bool user_gcr =
        (family_.format_features & AMDF_GPU_SDMA_FORMAT_FEATURE_USER_GCR) != 0;
    const size_t words_per_transfer = 17 + (user_gcr ? 10 : 0);
    GpuMemory* source = nullptr;
    GpuMemory* target = nullptr;
    GpuMemory* control = nullptr;
    ASSERT_NO_FATAL_FAILURE(
        CreateMemory(AMDF_MEMORY_ACCESS_READ, kAllocationBytes, &source));
    ASSERT_NO_FATAL_FAILURE(
        CreateMemory(AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE,
                     kAllocationBytes, &target));
    ASSERT_NO_FATAL_FAILURE(
        CreateMemory(AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE,
                     kControlBytes, &control));
    GpuCommandQueue* queue = nullptr;
    ASSERT_NO_FATAL_FAILURE(CreateQueue(&queue));
    ASSERT_GT(queue->words().size(),
              words_per_transfer * rectangles.size() * kSeeds.size());
    std::vector<uint8_t> expected_source(kAllocationBytes);
    std::vector<uint8_t> expected_target(kAllocationBytes);
    std::vector<uint8_t> expected_control(kControlBytes);
    std::vector<uint8_t> observed_source(kAllocationBytes);
    std::vector<uint8_t> observed_target(kAllocationBytes);
    std::vector<uint8_t> observed_control(kControlBytes);
    SdmaCommandWriter commands(queue->words().data(), family_.format_features);
    RecordProperty("element_bytes", element_bytes);
    RecordProperty("sdma_format_features", family_.format_features);
    RecordProperty("z_bit_count", ZBitCount());
    RecordProperty("geometries", rectangles.size());
    RecordProperty("epochs_per_geometry", kSeeds.size());
    RecordProperty("words_per_transfer", words_per_transfer);
    RecordProperty("source_checked_bytes_per_transfer", kAllocationBytes);
    RecordProperty("target_checked_bytes_per_transfer", kAllocationBytes);
    RecordProperty("control_checked_bytes_per_transfer", kControlBytes);
    RecordProperty("completed_transfers", 0);
    size_t copied_bytes = 0;
    size_t completed = 0;
    for (const auto& rectangle : rectangles) {
      ASSERT_NO_FATAL_FAILURE(CheckOwnedRegion(
          rectangle.source, rectangle.extent, element_bytes, kAllocationBytes));
      ASSERT_NO_FATAL_FAILURE(CheckOwnedRegion(
          rectangle.target, rectangle.extent, element_bytes, kAllocationBytes));
      for (const uint32_t seed : kSeeds) {
        SCOPED_TRACE(std::string(rectangle.name) +
                     " seed=" + std::to_string(seed));
        for (size_t i = 0; i < kAllocationBytes; ++i) {
          expected_source[i] = Pattern(seed, i);
          expected_target[i] = Pattern(seed ^ 0x5au, i);
        }
        for (size_t i = 0; i < kControlBytes; ++i) {
          expected_control[i] = Pattern(seed ^ 0x91u, i);
        }
        std::memcpy(source->host.pointer, expected_source.data(),
                    kAllocationBytes);
        std::memcpy(target->host.pointer, expected_target.data(),
                    kAllocationBytes);
        const uint32_t marker = static_cast<uint32_t>(completed + 1);
        std::memcpy(expected_control.data() + kCompletionOffset, &marker,
                    sizeof(marker));
        std::memcpy(control->host.pointer, expected_control.data(),
                    kControlBytes);
        *reinterpret_cast<uint32_t*>(
            static_cast<uint8_t*>(control->host.pointer) + kCompletionOffset) =
            0;
        const size_t row_bytes = size_t{element_bytes} * rectangle.extent.width;
        for (uint32_t slice = 0; slice < rectangle.extent.depth; ++slice) {
          for (uint32_t row = 0; row < rectangle.extent.height; ++row) {
            const uint64_t source_offset =
                RowByteOffset(rectangle.source, element_bytes, row, slice);
            const uint64_t target_offset =
                RowByteOffset(rectangle.target, element_bytes, row, slice);
            std::copy_n(expected_source.data() + source_offset, row_bytes,
                        expected_target.data() + target_offset);
            for (size_t byte = 0; byte < row_bytes; ++byte) {
              static_cast<uint8_t*>(
                  target->host.pointer)[target_offset + byte] =
                  expected_target[target_offset + byte] ^ 0xffu;
            }
          }
        }
        if (user_gcr) {
          commands.AcquireFromSystem();
        }
        commands.CopyLinearRect(
            source->device_address + rectangle.source.byte_offset,
            rectangle.source.layout,
            target->device_address + rectangle.target.byte_offset,
            rectangle.target.layout, rectangle.extent, element_log2);
        if (user_gcr) {
          commands.ReleaseToSystem();
        }
        commands.Fence32(control->device_address + kCompletionOffset, marker);
        ASSERT_EQ(commands.word_count(), (completed + 1) * words_per_transfer);
        ASSERT_NO_FATAL_FAILURE(
            queue->Publish(api_, gpu_api_, commands.word_count()));
        GpuWaitEqual<uint32_t>(
            reinterpret_cast<uintptr_t>(control->host.pointer) +
                kCompletionOffset,
            marker);
        // Capture complete backing before diagnostics or retirement can
        // intervene.
        std::memcpy(observed_source.data(), source->host.pointer,
                    kAllocationBytes);
        std::memcpy(observed_target.data(), target->host.pointer,
                    kAllocationBytes);
        std::memcpy(observed_control.data(), control->host.pointer,
                    kControlBytes);
        CheckBytes("source", observed_source, expected_source);
        CheckBytes("target", observed_target, expected_target);
        CheckBytes("control", observed_control, expected_control);
        EXPECT_NO_FATAL_FAILURE(queue->WaitRetired(api_));
        if (HasFailure()) {
          return;
        }
        copied_bytes +=
            row_bytes * rectangle.extent.height * rectangle.extent.depth;
        ++completed;
        RecordProperty("completed_transfers", completed);
        RecordProperty("copied_bytes", copied_bytes);
        RecordProperty("retired_byte_frontier",
                       completed * words_per_transfer * 4);
      }
    }
  }
};

TEST_F(SdmaRectangularCopyTest, ByteElements) { Run(0, CommonRectangles(1)); }
TEST_F(SdmaRectangularCopyTest, TwoByteElements) {
  Run(1, CommonRectangles(2));
}
TEST_F(SdmaRectangularCopyTest, FourByteElements) {
  Run(2, CommonRectangles(4));
}
TEST_F(SdmaRectangularCopyTest, EightByteElements) {
  Run(3, CommonRectangles(8));
}
TEST_F(SdmaRectangularCopyTest, SixteenByteElements) {
  Run(4, CommonRectangles(16));
}

TEST_F(SdmaRectangularCopyTest, ZCoordinatesUseAdvertisedWidth) {
  const uint32_t depth_count = 1u << ZBitCount();
  const Rectangle rectangle = {"last_slices",
                               {0, {0, 0, depth_count - 2, 4, 4}},
                               {0, {0, 0, depth_count - 1, 4, 4}},
                               {1, 1, 1}};
  Run(0, {&rectangle, 1});
}

TEST_F(SdmaRectangularCopyTest, DepthUsesAdvertisedWidth) {
  const Rectangle rectangle = {"full_depth",
                               {0, {0, 0, 0, 4, 4}},
                               {0, {0, 0, 0, 4, 4}},
                               {1, 1, 1u << ZBitCount()}};
  Run(0, {&rectangle, 1});
}

}  // namespace
