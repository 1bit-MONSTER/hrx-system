// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <cstdlib>
#include <cstring>

#include "iree/base/tracing.h"
#include "iree/testing/gtest.h"

namespace {

TEST(TracyTracingTest, ZoneContextsAndEventFamilies) {
  IREE_TRACE_ZONE_BEGIN_NAMED(outer, "outer");
  char name[] = "dynamic";
  IREE_TRACE_ZONE_BEGIN_NAMED_DYNAMIC(inner, name, sizeof(name) - 1);
  EXPECT_NE(outer, inner);
  memset(name, 'x', sizeof(name) - 1);
  IREE_TRACE_ZONE_APPEND_TEXT(inner, "annotation");
  IREE_TRACE_ZONE_APPEND_TEXT(inner, name, sizeof(name) - 1);
  IREE_TRACE_ZONE_APPEND_VALUE_I64(inner, 42);
  IREE_TRACE_ZONE_SET_COLOR(inner, 0x123456);
  IREE_TRACE_ZONE_END(inner);
  IREE_TRACE_ZONE_BEGIN_EXTERNAL(external, "model.loom", 10, 7, "main", 4,
                                 "dispatch", 8);
  EXPECT_NE(outer, external);
  EXPECT_NE(inner, external);
  IREE_TRACE_ZONE_END(external);

  IREE_TRACE_MESSAGE(ERROR, "error");
  IREE_TRACE_MESSAGE(WARNING, "warning");
  IREE_TRACE_MESSAGE(INFO, "info");
  IREE_TRACE_MESSAGE(VERBOSE, "verbose");
  IREE_TRACE_MESSAGE(DEBUG, "debug");
  IREE_TRACE_MESSAGE_DYNAMIC(ERROR, name, sizeof(name) - 1);
  IREE_TRACE_MESSAGE_DYNAMIC(WARNING, name, sizeof(name) - 1);
  IREE_TRACE_MESSAGE_DYNAMIC(INFO, name, sizeof(name) - 1);
  IREE_TRACE_MESSAGE_DYNAMIC(VERBOSE, name, sizeof(name) - 1);
  IREE_TRACE_MESSAGE_DYNAMIC(DEBUG, name, sizeof(name) - 1);
  IREE_TRACE_MESSAGE_COLORED(0x123456, "colored");
  IREE_TRACE_MESSAGE_DYNAMIC_COLORED(0x123456, name, sizeof(name) - 1);

  void* allocation = malloc(16);
  EXPECT_NE(allocation, nullptr);
  IREE_TRACE_ALLOC(allocation, 16);
  IREE_TRACE_FREE(allocation);
  IREE_TRACE_ALLOC_NAMED("pool", allocation, 16);
  IREE_TRACE_FREE_NAMED("pool", allocation);
  free(allocation);

  IREE_TRACE_SET_PLOT_TYPE("values", IREE_TRACING_PLOT_TYPE_NUMBER, false,
                           false, 0x123456);
  IREE_TRACE_PLOT_VALUE_I64("values", 42);
  IREE_TRACE_PLOT_VALUE_F32("values", 1.5f);
  IREE_TRACE_PLOT_VALUE_F64("values", 2.5);
  IREE_TRACE_FRAME_MARK();
  IREE_TRACE_FRAME_MARK_NAMED("frames");
  IREE_TRACE_FRAME_MARK_BEGIN_NAMED("intervals");
  IREE_TRACE_FRAME_MARK_END_NAMED("intervals");
  IREE_TRACE_ZONE_END(outer);
}

}  // namespace

#if !defined(IREE_PLATFORM_WINDOWS) && !defined(IREE_PLATFORM_WASM)
#include <pthread.h>
#include <signal.h>

#include "client/TracyThread.hpp"

namespace {

TEST(TracyTracingTest, WorkerPreservesApplicationSignalOwnership) {
  sigset_t creator_mask;
  ASSERT_EQ(pthread_sigmask(SIG_SETMASK, nullptr, &creator_mask), 0);
  struct {
    // Signal mask observed by the worker before doing any other work.
    sigset_t mask;
    // Result from querying the worker's signal mask.
    int query_result;
  } worker_state = {};
  {
    tracy::Thread worker(
        [](void* user_data) {
          auto* state = static_cast<decltype(worker_state)*>(user_data);
          state->query_result =
              pthread_sigmask(SIG_SETMASK, nullptr, &state->mask);
        },
        &worker_state);
  }
  ASSERT_EQ(worker_state.query_result, 0);
  sigset_t restored_mask;
  ASSERT_EQ(pthread_sigmask(SIG_SETMASK, nullptr, &restored_mask), 0);
  for (int signal = 1; signal < NSIG; ++signal) {
    EXPECT_EQ(sigismember(&restored_mask, signal),
              sigismember(&creator_mask, signal))
        << signal;
  }
#if defined(IREE_PLATFORM_LINUX) && !defined(TRACY_NO_CRASH_HANDLER)
#if defined(TRACY_CRASH_SIGNAL)
  const int crash_signal = TRACY_CRASH_SIGNAL;
#else
  const int crash_signal = SIGPWR;
#endif
#else
  const int crash_signal = 0;
#endif
  for (int signal : {SIGINT, SIGTERM, SIGHUP, SIGQUIT, SIGUSR1, SIGUSR2}) {
    if (signal != crash_signal) {
      EXPECT_EQ(sigismember(&worker_state.mask, signal), 1) << signal;
    }
  }
  for (int signal :
       {SIGILL, SIGFPE, SIGSEGV, SIGBUS, SIGABRT, SIGTRAP, SIGSYS}) {
    EXPECT_EQ(sigismember(&worker_state.mask, signal),
              sigismember(&creator_mask, signal))
        << signal;
  }
  if (crash_signal) {
    EXPECT_EQ(sigismember(&worker_state.mask, crash_signal),
              sigismember(&creator_mask, crash_signal));
  }
}

}  // namespace
#endif  // !IREE_PLATFORM_WINDOWS && !IREE_PLATFORM_WASM
