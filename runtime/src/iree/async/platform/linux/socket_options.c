// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/async/platform/linux/socket_options.h"

#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <time.h>

// Linux UAPI value, also used when libc headers predate this socket option.
#ifndef TCP_DELACK_MAX_US
#define TCP_DELACK_MAX_US 46
#endif  // TCP_DELACK_MAX_US

iree_status_t iree_async_linux_socket_set_low_latency_ack(int fd) {
  // CLOCK_MONOTONIC_COARSE reports the kernel timer tick, unlike _SC_CLK_TCK
  // (userspace accounting frequency). TCP requires at least two timer ticks.
  struct timespec resolution;
  if (clock_getres(CLOCK_MONOTONIC_COARSE, &resolution) != 0) {
    return iree_make_status(iree_status_code_from_errno(errno),
                            "clock_getres CLOCK_MONOTONIC_COARSE failed");
  }
  int64_t tick_ns =
      (int64_t)resolution.tv_sec * 1000000000 + resolution.tv_nsec;
  // Floor the conversion: rounding up a fractional tick (such as HZ=300)
  // could cause the kernel's usecs_to_jiffies to select a third tick.
  int cap_us = (int)iree_max(2000, 2 * tick_ns / 1000);
  if (setsockopt(fd, IPPROTO_TCP, TCP_DELACK_MAX_US, &cap_us, sizeof(cap_us)) !=
          0 &&
      errno != ENOPROTOOPT) {
    return iree_make_status(iree_status_code_from_errno(errno),
                            "setsockopt TCP_DELACK_MAX_US failed");
  }
  return iree_ok_status();
}
