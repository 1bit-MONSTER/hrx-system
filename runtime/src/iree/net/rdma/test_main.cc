// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/async/cts/util/registry.h"
#include "iree/net/rdma/test_context.h"
#include "iree/testing/gtest.h"

int main(int argc, char** argv) {
  IREE_TRACE_APP_ENTER();
  iree::async::cts::CtsRegistry::InstantiateAll();
  ::testing::InitGoogleTest(&argc, argv);
  int result = 0;
  {
#if defined(IREE_NET_RDMA_TEST_CONNECTION)
    constexpr auto kind = iree::net::rdma::TestContextKind::kConnection;
#else
    constexpr auto kind = iree::net::rdma::TestContextKind::kLocal;
#endif
    iree::net::rdma::TestContextEnvironment environment(kind);
    result = RUN_ALL_TESTS();
  }
  IREE_TRACE_APP_EXIT(result);
  return result;
}
