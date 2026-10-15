// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/base/time.h"

#include "iree/testing/gtest.h"

namespace iree {
namespace {

TEST(TimeTest, RelativeTimeoutSentinels) {
  EXPECT_EQ(iree_relative_timeout_to_deadline_ns(IREE_DURATION_ZERO),
            IREE_TIME_INFINITE_PAST);
  EXPECT_EQ(iree_relative_timeout_to_deadline_ns(IREE_DURATION_INFINITE),
            IREE_TIME_INFINITE_FUTURE);
}

TEST(TimeTest, RelativeTimeoutUsesMonotonicTime) {
  const iree_duration_t durations[] = {1, 1000000, -1, -1000000, INT64_MIN};
  for (iree_duration_t duration : durations) {
    SCOPED_TRACE(duration);
    const iree_time_t before = iree_time_now();
    const iree_time_t deadline = iree_relative_timeout_to_deadline_ns(duration);
    const iree_time_t after = iree_time_now();
    EXPECT_GE(deadline, before + duration);
    EXPECT_LE(deadline, after + duration);
  }
}

TEST(TimeTest, LargeRelativeTimeoutStaysFinite) {
  iree_timeout_t timeout = iree_make_timeout_ns(IREE_DURATION_INFINITE - 1);
  EXPECT_FALSE(iree_timeout_is_infinite(timeout));
  iree_convert_timeout_to_absolute(&timeout);
  EXPECT_EQ(timeout.nanos, IREE_TIME_INFINITE_FUTURE - 1);
  EXPECT_FALSE(iree_timeout_is_infinite(timeout));
  EXPECT_GT(iree_timeout_as_duration_ns(timeout), 0);
}

TEST(TimeTest, MinimumTimeoutPreservesFiniteDeadline) {
  const iree_timeout_t timeout =
      iree_timeout_min(iree_infinite_timeout(),
                       iree_make_timeout_ns(IREE_DURATION_INFINITE - 1));
  EXPECT_EQ(timeout.nanos, IREE_TIME_INFINITE_FUTURE - 1);
  EXPECT_FALSE(iree_timeout_is_infinite(timeout));
}

}  // namespace
}  // namespace iree
