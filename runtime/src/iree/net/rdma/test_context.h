// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef IREE_NET_RDMA_TEST_CONTEXT_H_
#define IREE_NET_RDMA_TEST_CONTEXT_H_

#include <array>

#include "iree/net/rdma/context.h"

namespace iree::net::rdma {

// Local verbs tests and routed connection tests have independent selections.
enum class TestContextKind { kLocal, kConnection };

// Explicit main-scoped owner shared by every case, repetition, and trial in one
// executable. Two contexts provide independent PDs on the selected device.
// Device discovery and open/close never happen on a per-test acquisition path.
// Construction records optional-provider failures for individual tests to skip;
// an explicitly selected unavailable device is a configuration failure.
class TestContextEnvironment {
 public:
  explicit TestContextEnvironment(TestContextKind kind);
  ~TestContextEnvironment();
  TestContextEnvironment(const TestContextEnvironment&) = delete;
  TestContextEnvironment& operator=(const TestContextEnvironment&) = delete;

  // Returns an independent retained reference to context zero or one. The main
  // owner remains alive until all test/trial references and work are retired.
  static iree_status_t Acquire(uint32_t index,
                               iree_net_rdma_context_t** out_context);

 private:
  // Setup failure retained for native cases without hiding pure software tests.
  iree::Status status_;
  // Fixed primary/alternate protection-domain owners, released at process end.
  std::array<iree_net_rdma_context_t*, 2> contexts_ = {};
};

}  // namespace iree::net::rdma

#endif  // IREE_NET_RDMA_TEST_CONTEXT_H_
