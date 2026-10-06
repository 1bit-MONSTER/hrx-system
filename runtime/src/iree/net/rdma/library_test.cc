// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/net/rdma/library.h"

#include <dlfcn.h>

#include <cstdlib>

#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"

namespace iree::net::rdma {
namespace {

TEST(LibraryTest, NativeInventoryModulesOutliveLibraryOwners) {
  iree_net_rdma_library_t library = {};
  iree_status_t status =
      iree_net_rdma_library_initialize(iree_allocator_system(), &library);
  if (iree_status_is_not_found(status) &&
      !std::getenv("IREE_NET_RDMA_TEST_DEVICE") &&
      !std::getenv("IREE_NET_RDMA_CM_TEST_DEVICE") &&
      !std::getenv("IREE_NET_RDMA_CM_TEST_ADDRESS")) {
    iree_status_free(status);
    GTEST_SKIP() << "Native RDMA libraries are not available.";
  }
  IREE_ASSERT_OK(status);
  iree_net_rdma_library_deinitialize(&library);

  // NOLOAD observes residency without loading a new module. No native device
  // is opened: the contract must hold even before CM initializes its inventory.
  for (const char* name : {"librdmacm.so.1", "libibverbs.so.1"}) {
    void* handle = dlopen(name, RTLD_LAZY | RTLD_NOLOAD);
    EXPECT_NE(handle, nullptr) << name;
    if (handle) {
      EXPECT_EQ(dlclose(handle), 0);
    }
  }
}

}  // namespace
}  // namespace iree::net::rdma
