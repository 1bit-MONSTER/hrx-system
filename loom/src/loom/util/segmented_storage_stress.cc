// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/util/segmented_storage_test_util.h"

namespace loom {
namespace {

using testing::SegmentedStorageAppendCase;
using testing::SegmentedStorageTest;

TEST_F(SegmentedStorageTest, EveryAppendFailureCanRewindAndRetry) {
  const uint32_t boundaries[] = {
      0,
      LOOM_SEGMENTED_STORAGE_INLINE_SEGMENT_COUNT,
      LOOM_SEGMENTED_STORAGE_SEGMENTS_PER_PAGE,
      LOOM_SEGMENTED_STORAGE_SEGMENTS_PER_PAGE + 1,
      2 * LOOM_SEGMENTED_STORAGE_SEGMENTS_PER_PAGE,
      LOOM_SEGMENTED_STORAGE_SEGMENTS_PER_PAGE *
          LOOM_SEGMENTED_STORAGE_SEGMENTS_PER_PAGE,
  };
  CheckAppendFailuresAtBoundaries(boundaries, IREE_ARRAYSIZE(boundaries));
}

TEST_F(SegmentedStorageTest, EveryDiscardedExtensionPreservesPrefixAndRetries) {
  constexpr uint32_t kInline = LOOM_SEGMENTED_STORAGE_INLINE_SEGMENT_COUNT;
  constexpr uint32_t kPage = LOOM_SEGMENTED_STORAGE_SEGMENTS_PER_PAGE;
  const SegmentedStorageAppendCase cases[] = {
      {0, 4},
      {kInline - 1, kInline + 3},
      {kInline, kInline + 4},
      {kInline + 1, kInline + 5},
      {kPage - 1, kPage + 3},
      {kPage, kPage + 4},
      {kPage + 1, 2 * kPage + 3},
      {2 * kPage, 2 * kPage + 4},
      {kPage * kPage - 1, kPage * kPage + 3},
      {kPage * kPage, kPage * kPage + 4},
  };
  CheckDiscardedExtensionCases(cases, IREE_ARRAYSIZE(cases));
}

}  // namespace
}  // namespace loom
