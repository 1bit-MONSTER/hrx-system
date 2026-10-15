// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "util/wait.h"

#include <atomic>
#include <string>

#include "util/native_event.h"

namespace amdf::cts {
namespace {

TEST(WaitTest, ReadyObservationNeedsNoBudget) {
  ASSERT_TRUE(WaitUntil("already ready", [] { return true; }, WaitDeadline(0)));
}

TEST(WaitTest, MissingProgressReturnsItsDescription) {
  uint32_t observations = 0;
  const auto result = WaitUntil(
      "sample 42",
      [&] {
        ++observations;
        return false;
      },
      WaitDeadline(0));
  EXPECT_FALSE(result);
  EXPECT_EQ(observations, 1u);
  EXPECT_NE(std::string(result.message()).find("sample 42"), std::string::npos);
}

TEST(WaitTest, PollingSleepsBetweenObservations) {
  const auto start = std::chrono::steady_clock::now();
  uint32_t observations = 0;
  ASSERT_TRUE(
      WaitUntil("second observation", [&] { return ++observations == 2; }));
  EXPECT_GE(std::chrono::steady_clock::now() - start,
            std::chrono::nanoseconds(kWaitPollIntervalNs));
}

TEST(WaitTest, DelayedProducerCanComplete) {
  std::atomic<bool> ready = false;
  std::jthread producer([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    ready.store(true, std::memory_order_release);
  });
  ASSERT_TRUE(WaitUntil("producer",
                        [&] { return ready.load(std::memory_order_acquire); }));
}

TEST(WaitTest, RetriesShareAnAbsoluteBudget) {
  WaitDeadline deadline(1'000'000);
  const auto copy = deadline;
  ASSERT_FALSE(WaitUntil("missing producer", [] { return false; }, deadline));
  EXPECT_EQ(copy.remaining_ns(), 0u);
  EXPECT_EQ(copy.remaining_ms(), 0u);
  EXPECT_FALSE(copy.Pause());
}

TEST(WaitTest, NativeEventWithoutProducerTimesOut) {
  amdf_native_event_t event = {};
  ASSERT_TRUE(CreateNativeEvent(&event));
  const auto result = WaitNativeEvent(event, WaitDeadline(1'000'000));
  EXPECT_FALSE(result);
  EXPECT_NE(std::string(result.message()).find("timed out"), std::string::npos);
  EXPECT_TRUE(DestroyNativeEvent(&event));
}

}  // namespace
}  // namespace amdf::cts
