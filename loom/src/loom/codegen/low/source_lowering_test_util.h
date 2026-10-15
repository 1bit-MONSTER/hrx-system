// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_CODEGEN_LOW_SOURCE_LOWERING_TEST_UTIL_H_
#define LOOM_CODEGEN_LOW_SOURCE_LOWERING_TEST_UTIL_H_

#include "iree/base/internal/arena.h"
#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"
#include "loom/codegen/low/testing/source_workload.h"
#include "loom/codegen/low/testing/source_workload_pipeline.h"
#include "loom/ir/context.h"
#include "loom/pass/builtin_registry.h"
#include "loom/target/test/low_registry.h"
#include "loom/target/test/lower.h"

namespace loom::testing {

class SourceLoweringTestBase : public ::testing::Test {
 protected:
  void SetUp() override {
    iree_arena_block_pool_initialize(4096, iree_allocator_system(),
                                     &block_pool_);
    loom_context_initialize(iree_allocator_system(), &context_);
    IREE_ASSERT_OK(loom_low_source_workload_register_dialects(&context_));
    IREE_ASSERT_OK(loom_context_finalize(&context_));
    loom_test_low_descriptor_registry_initialize(&descriptor_registry_);
    loom_test_low_lower_policy_registry_initialize(&policy_registry_);
  }

  void TearDown() override {
    loom_context_deinitialize(&context_);
    iree_arena_block_pool_deinitialize(&block_pool_);
  }

  // Arena storage shared across one generated-workload test case.
  iree_arena_block_pool_t block_pool_;
  // Dialect context used to construct the generated module.
  loom_context_t context_;
  // Descriptor registry for the synthetic low target.
  loom_target_low_descriptor_registry_t descriptor_registry_ = {};
  // Lowering policy registry for the synthetic low target.
  loom_low_lower_policy_registry_t policy_registry_ = {};
};

}  // namespace loom::testing

#endif  // LOOM_CODEGEN_LOW_SOURCE_LOWERING_TEST_UTIL_H_
