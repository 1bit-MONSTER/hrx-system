// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <string>

#include "iree/net/carrier/stream/carrier_test_suite.h"

namespace iree::net::testing {

namespace {

// Socket names must fit sockaddr_un even when the runner's artifact directory
// does not. The connection setup owns a unique short filesystem path.
struct UnixSocketPath {
  // Private directory owned by connection setup.
  std::string directory;

  // Socket node within |directory|, created by bind.
  std::string path;

  ~UnixSocketPath() {
    if (unlink(path.c_str()) != 0) {
      // Setup may have failed before bind created the node.
      EXPECT_EQ(errno, ENOENT);
    }
    EXPECT_EQ(rmdir(directory.c_str()), 0);
  }
};

}  // namespace

void StreamCarrierTest::EstablishSockets(
    iree_async_socket_t** out_client_socket,
    iree_async_socket_t** out_server_socket) {
  char directory[] = "/tmp/iree-stream-XXXXXX";
  ASSERT_NE(mkdtemp(directory), nullptr);
  UnixSocketPath socket_path{directory, std::string(directory) + "/socket"};
  iree_async_address_t bind_address;
  IREE_ASSERT_OK(iree_async_address_from_unix(
      iree_make_cstring_view(socket_path.path.c_str()), &bind_address));
  ASSERT_NO_FATAL_FAILURE(
      EstablishSocketsAtAddress(bind_address, GetParam().socket_options,
                                out_client_socket, out_server_socket));
}

INSTANTIATE_TEST_SUITE_P(
    Unix, StreamCarrierTest,
    ::testing::Values(StreamSendMode{
        "Copied", IREE_ASYNC_SOCKET_TYPE_UNIX_STREAM,
        IREE_ASYNC_SOCKET_OPTION_NONE, IREE_HOST_SIZE_MAX}),
    [](const ::testing::TestParamInfo<StreamSendMode>& info) {
      return info.param.name;
    });

}  // namespace iree::net::testing
