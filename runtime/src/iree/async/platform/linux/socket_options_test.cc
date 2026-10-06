// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/async/platform/linux/socket_options.h"

#include <netinet/in.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <cerrno>

#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"

namespace {

// Interpose the helper's native dependencies only on the calling test thread.
// Socket CTS separately exercises the real kernel and both proactor backends.
struct NativeOptions {
  // Socket whose option calls are intercepted.
  int fd = -1;
  // Simulated kernel timer resolution, in nanoseconds.
  long tick_nanoseconds = 1000000;
  // Native error returned by clock_getres, or zero for success.
  int clock_error = 0;
  // Native error returned by setsockopt, or zero for success.
  int option_error = 0;
  // Number of option attempts for this socket.
  int option_calls = 0;
  // Last cap requested by the production helper, in microseconds.
  int cap_microseconds = 0;
};
thread_local NativeOptions* native_options = nullptr;

}  // namespace

extern "C" int __real_clock_getres(clockid_t clock,
                                   struct timespec* resolution);
extern "C" int __wrap_clock_getres(clockid_t clock,
                                   struct timespec* resolution) {
  if (!native_options || clock != CLOCK_MONOTONIC_COARSE) {
    return __real_clock_getres(clock, resolution);
  }
  if (native_options->clock_error) {
    errno = native_options->clock_error;
    return -1;
  }
  resolution->tv_sec = 0;
  resolution->tv_nsec = native_options->tick_nanoseconds;
  return 0;
}

extern "C" int __real_setsockopt(int fd, int level, int option,
                                 const void* value, socklen_t length);
extern "C" int __wrap_setsockopt(int fd, int level, int option,
                                 const void* value, socklen_t length) {
  if (!native_options || fd != native_options->fd) {
    return __real_setsockopt(fd, level, option, value, length);
  }
  EXPECT_EQ(level, IPPROTO_TCP);
  EXPECT_EQ(option, 46);  // Linux TCP_DELACK_MAX_US UAPI value.
  EXPECT_EQ(length, sizeof(int));
  ++native_options->option_calls;
  native_options->cap_microseconds = *static_cast<const int*>(value);
  if (native_options->option_error) {
    errno = native_options->option_error;
    return -1;
  }
  return 0;
}

namespace {

class SocketOptionsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    options_.fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ASSERT_GE(options_.fd, 0);
    native_options = &options_;
  }
  void TearDown() override {
    native_options = nullptr;
    if (options_.fd >= 0) {
      close(options_.fd);
    }
  }

  // Native dependency inputs and observed setup calls for this test.
  NativeOptions options_;
};

TEST_F(SocketOptionsTest, RespectsKernelTickFloor) {
  struct {
    // Kernel coarse-clock resolution.
    long tick_nanoseconds;
    // Request that rounds to at least two kernel ticks and targets 2 ms.
    int cap_microseconds;
  } cases[] = {{10000000, 20000},
               {4000000, 8000},
               {3333333, 6666},
               {1000000, 2000},
               {500000, 2000}};
  for (const auto& test_case : cases) {
    SCOPED_TRACE(test_case.tick_nanoseconds);
    options_.tick_nanoseconds = test_case.tick_nanoseconds;
    options_.option_calls = 0;
    IREE_EXPECT_OK(iree_async_linux_socket_set_low_latency_ack(options_.fd));
    EXPECT_EQ(options_.cap_microseconds, test_case.cap_microseconds);
    EXPECT_EQ(options_.option_calls, 1);
  }
}

TEST_F(SocketOptionsTest, UnsupportedKernelAcceptsHint) {
  options_.option_error = ENOPROTOOPT;
  IREE_EXPECT_OK(iree_async_linux_socket_set_low_latency_ack(options_.fd));
  EXPECT_EQ(options_.option_calls, 1);
}

TEST_F(SocketOptionsTest, UnexpectedOptionErrorsPropagateWithoutRetry) {
  for (int error : {EINVAL, EPERM, ENOBUFS}) {
    SCOPED_TRACE(error);
    options_.option_error = error;
    options_.option_calls = 0;
    IREE_EXPECT_STATUS_IS(
        iree_status_code_from_errno(error),
        iree_async_linux_socket_set_low_latency_ack(options_.fd));
    EXPECT_EQ(options_.option_calls, 1);
  }
}

TEST_F(SocketOptionsTest, ClockFailurePreventsOptionAttempt) {
  options_.clock_error = EIO;
  IREE_EXPECT_STATUS_IS(
      iree_status_code_from_errno(EIO),
      iree_async_linux_socket_set_low_latency_ack(options_.fd));
  EXPECT_EQ(options_.option_calls, 0);
}

}  // namespace
