// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/net/rdma/test_context.h"

#include <cstdlib>

namespace iree::net::rdma {
namespace {

// Published by main before test threads start; cleared after they all join.
TestContextEnvironment* environment = nullptr;

}  // namespace

TestContextEnvironment::TestContextEnvironment(TestContextKind kind) {
  IREE_ASSERT(!environment);
  environment = this;
  if (kind == TestContextKind::kConnection &&
      !std::getenv("IREE_NET_RDMA_CM_TEST_ADDRESS")) {
    status_ =
        iree_make_status(IREE_STATUS_UNAVAILABLE,
                         "set IREE_NET_RDMA_CM_TEST_ADDRESS and "
                         "IREE_NET_RDMA_CM_TEST_DEVICE for native RDMA CTS");
    return;
  }
  const char* device = std::getenv(kind == TestContextKind::kLocal
                                       ? "IREE_NET_RDMA_TEST_DEVICE"
                                       : "IREE_NET_RDMA_CM_TEST_DEVICE");
  auto options = iree_net_rdma_context_options_default();
  options.device_name = iree_make_cstring_view(device);
  for (size_t i = 0; i < contexts_.size() && status_.ok(); ++i) {
    status_ = iree_net_rdma_context_create(options, iree_allocator_system(),
                                           &contexts_[i]);
  }
  if (iree_status_is_unavailable(status_.get()) &&
      (device || kind == TestContextKind::kConnection)) {
    status_ = iree_status_join(
        iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                         "explicitly requested RDMA context is unavailable"),
        status_.release());
  }
}

TestContextEnvironment::~TestContextEnvironment() {
  for (auto* context : contexts_) {
    iree_net_rdma_context_release(context);
  }
  environment = nullptr;
}

iree_status_t TestContextEnvironment::Acquire(
    uint32_t index, iree_net_rdma_context_t** out_context) {
  IREE_ASSERT(environment && index < environment->contexts_.size());
  *out_context = nullptr;
  IREE_RETURN_IF_ERROR(iree_status_clone(environment->status_.get()));
  *out_context = environment->contexts_[index];
  iree_net_rdma_context_retain(*out_context);
  return iree_ok_status();
}

}  // namespace iree::net::rdma
