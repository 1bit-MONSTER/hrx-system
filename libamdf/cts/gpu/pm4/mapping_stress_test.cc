// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "libamdf/cts/gpu/pm4/mapping_fixture.h"

namespace {

class Pm4MappingStressTest : public Pm4MappingFixture {};

TEST_F(Pm4MappingStressTest,
       SystemBackingPermutesRetainedSparseStateAndCacheRanges) {
  const auto original_ranges = amdf::testing::ModelSparseRanges(1);
  const auto replacement_ranges = amdf::testing::ModelSparseRanges(2);
  RunRetainedSparse(system_scope_, original_ranges, replacement_ranges);
}

TEST_F(Pm4MappingStressTest,
       LocalBackingPermutesRetainedSparseStateAndCacheRanges) {
  if (local_scope_ == nullptr) {
    GTEST_SKIP() << "endpoint has no local storage scope";
  }
  const auto original_ranges = amdf::testing::ModelSparseRanges(1);
  const auto replacement_ranges = amdf::testing::ModelSparseRanges(2);
  RunRetainedSparse(local_scope_, original_ranges, replacement_ranges);
}

}  // namespace
