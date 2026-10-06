// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef IREE_ASYNC_PLATFORM_LINUX_SOCKET_OPTIONS_H_
#define IREE_ASYNC_PLATFORM_LINUX_SOCKET_OPTIONS_H_

#include "iree/base/api.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

// Requests a persistent low delayed-ACK cap on a TCP socket before connect or
// listen. Accepted sockets inherit the listener's cap. Kernels without the
// option retain their default ACK policy; other native setup errors propagate.
// Does not change descriptor ownership or require per-message socket calls.
iree_status_t iree_async_linux_socket_set_low_latency_ack(int fd);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // IREE_ASYNC_PLATFORM_LINUX_SOCKET_OPTIONS_H_
