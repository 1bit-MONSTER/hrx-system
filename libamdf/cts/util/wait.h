// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef AMDF_CTS_UTIL_WAIT_H_
#define AMDF_CTS_UTIL_WAIT_H_

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <thread>

#include "gtest/gtest.h"

namespace amdf::cts {

// Correctness waits have enough headroom for instrumented execution while
// returning control to the fixture before the outer test runner kills it.
inline constexpr uint64_t kWaitTimeoutNs = 10'000'000'000;
inline constexpr uint64_t kWaitPollIntervalNs = 50'000;

// One monotonic budget, shared across retries and spurious notifications.
// Expiration reports failed progress; it never establishes resource retirement.
class WaitDeadline {
 public:
  explicit WaitDeadline(uint64_t timeout_ns = kWaitTimeoutNs)
      : deadline_(std::chrono::steady_clock::now() +
                  std::chrono::nanoseconds(timeout_ns)) {}

  uint64_t remaining_ns() const {
    const auto remaining = std::chrono::duration_cast<std::chrono::nanoseconds>(
        deadline_ - std::chrono::steady_clock::now());
    return remaining.count() > 0 ? static_cast<uint64_t>(remaining.count()) : 0;
  }

  // Native millisecond waits round up without extending the absolute budget.
  uint32_t remaining_ms() const {
    return static_cast<uint32_t>((remaining_ns() + 999'999) / 1'000'000);
  }

  // Poll only when the producer has no native notification mechanism. Sleep
  // between observations rather than burning a CPU while the device progresses.
  [[nodiscard]] bool Pause() const {
    const uint64_t remaining = remaining_ns();
    if (remaining == 0) {
      return false;
    }
    std::this_thread::sleep_for(
        std::chrono::nanoseconds(std::min(remaining, kWaitPollIntervalNs)));
    return true;
  }

 private:
  // Absolute end of this wait, independent of retries or partial progress.
  std::chrono::steady_clock::time_point deadline_;
};

// Predicates perform a nonblocking observation. Callers append the last
// observed protocol state to a failed assertion for actionable diagnostics.
template <typename Predicate>
[[nodiscard]] ::testing::AssertionResult WaitUntil(
    const char* description, Predicate predicate,
    WaitDeadline deadline = WaitDeadline()) {
  while (!predicate()) {
    if (!deadline.Pause()) {
      return ::testing::AssertionFailure()
             << "timed out waiting for " << description;
    }
  }
  return ::testing::AssertionSuccess();
}

}  // namespace amdf::cts

#endif  // AMDF_CTS_UTIL_WAIT_H_
