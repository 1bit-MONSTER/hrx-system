// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/net/carrier/stream/carrier_test_suite.h"

namespace iree::net::testing {

void StreamCarrierTest::EstablishSockets(
    iree_async_socket_t** out_client_socket,
    iree_async_socket_t** out_server_socket) {
  iree_async_address_t bind_address;
  if (GetParam().socket_type == IREE_ASYNC_SOCKET_TYPE_TCP6) {
    IREE_ASSERT_OK(
        iree_async_address_from_ipv6(IREE_SV("::1"), 0, &bind_address));
  } else {
    IREE_ASSERT_OK(
        iree_async_address_from_ipv4(IREE_SV("127.0.0.1"), 0, &bind_address));
  }
  ASSERT_NO_FATAL_FAILURE(EstablishSocketsAtAddress(
      bind_address,
      GetParam().socket_options | IREE_ASYNC_SOCKET_OPTION_REUSE_ADDR,
      out_client_socket, out_server_socket));
}

static constexpr StreamSendMode kStreamSendModes[] = {
    StreamSendMode{"IPv4Copied", IREE_ASYNC_SOCKET_TYPE_TCP,
                   IREE_ASYNC_SOCKET_OPTION_NONE,
                   IREE_NET_STREAM_DEFAULT_ZERO_COPY_MIN_SEND_SIZE},
    StreamSendMode{"IPv4ZeroCopy", IREE_ASYNC_SOCKET_TYPE_TCP,
                   IREE_ASYNC_SOCKET_OPTION_ZERO_COPY, 0},
    StreamSendMode{"IPv4Adaptive", IREE_ASYNC_SOCKET_TYPE_TCP,
                   IREE_ASYNC_SOCKET_OPTION_ZERO_COPY,
                   IREE_NET_STREAM_DEFAULT_ZERO_COPY_MIN_SEND_SIZE},
    StreamSendMode{"IPv4CopiedOverride", IREE_ASYNC_SOCKET_TYPE_TCP,
                   IREE_ASYNC_SOCKET_OPTION_ZERO_COPY, IREE_HOST_SIZE_MAX},
    StreamSendMode{"IPv6Copied", IREE_ASYNC_SOCKET_TYPE_TCP6,
                   IREE_ASYNC_SOCKET_OPTION_NONE,
                   IREE_NET_STREAM_DEFAULT_ZERO_COPY_MIN_SEND_SIZE},
    StreamSendMode{"IPv6ZeroCopy", IREE_ASYNC_SOCKET_TYPE_TCP6,
                   IREE_ASYNC_SOCKET_OPTION_ZERO_COPY, 0},
    StreamSendMode{"IPv6Adaptive", IREE_ASYNC_SOCKET_TYPE_TCP6,
                   IREE_ASYNC_SOCKET_OPTION_ZERO_COPY,
                   IREE_NET_STREAM_DEFAULT_ZERO_COPY_MIN_SEND_SIZE},
    StreamSendMode{"IPv6CopiedOverride", IREE_ASYNC_SOCKET_TYPE_TCP6,
                   IREE_ASYNC_SOCKET_OPTION_ZERO_COPY, IREE_HOST_SIZE_MAX},
};

INSTANTIATE_TEST_SUITE_P(
    SendModes, StreamCarrierTest, ::testing::ValuesIn(kStreamSendModes),
    [](const ::testing::TestParamInfo<StreamSendMode>& info) {
      return info.param.name;
    });

}  // namespace iree::net::testing
