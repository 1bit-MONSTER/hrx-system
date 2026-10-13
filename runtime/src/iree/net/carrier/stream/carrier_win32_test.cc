// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <winsock2.h>

#include "iree/net/carrier/stream/carrier_test_suite.h"

namespace iree::net::testing {

void StreamCarrierTest::ConstrainSendBuffer(iree_async_socket_t* socket) {
  int send_buffer_size = 1;
  ASSERT_EQ(
      setsockopt(static_cast<SOCKET>(socket->primitive.value.win32_handle),
                 SOL_SOCKET, SO_SNDBUF,
                 reinterpret_cast<const char*>(&send_buffer_size),
                 sizeof(send_buffer_size)),
      0);
}

}  // namespace iree::net::testing
