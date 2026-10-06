// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "benchmark/benchmark.h"
#include "iree/async/cts/util/registry.h"
#include "iree/net/rdma/test_context.h"
#include "iree/testing/benchmark.h"

int main(int argc, char** argv) {
  IREE_TRACE_APP_ENTER();
  iree::async::cts::CtsRegistry::InstantiateAll();
  iree_benchmark_initialize(&argc, argv);
  if (::benchmark::ReportUnrecognizedArguments(argc, argv)) {
    IREE_TRACE_APP_EXIT(1);
    return 1;
  }
  int result = 0;
  {
    iree::net::rdma::TestContextEnvironment environment(
        iree::net::rdma::TestContextKind::kConnection);
    result = iree_benchmark_run_specified() ? 0 : 1;
    ::benchmark::Shutdown();
  }
  IREE_TRACE_APP_EXIT(result);
  return result;
}
