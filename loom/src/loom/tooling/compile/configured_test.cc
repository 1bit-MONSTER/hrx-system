// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/tooling/compile/configured.h"

#include "iree/testing/gtest.h"
#include "loom/target/configured/compiler_provider_set.h"
#include "loom/target/configured/provider_set.h"

namespace loom {
namespace {

TEST(ConfiguredCompileTest, ReturnsStableCompleteEnvironment) {
  const loom_tooling_compile_environment_t* environment =
      loom_tooling_configured_compile_environment();
  ASSERT_NE(environment, nullptr);
  EXPECT_EQ(environment, loom_tooling_configured_compile_environment());
  ASSERT_NE(environment->target_environment, nullptr);
  EXPECT_NE(environment->cleanup_pattern_provider_set, nullptr);

  const loom_target_provider_set_t* target_provider_set =
      loom_configured_target_provider_set();
  const loom_target_provider_set_t* emitter_provider_set =
      loom_configured_emitter_provider_set();
  const loom_target_provider_set_t* environment_provider_set =
      environment->target_environment->provider_set;
  ASSERT_EQ(environment_provider_set->provider_count,
            target_provider_set->provider_count +
                emitter_provider_set->provider_count);
  for (iree_host_size_t i = 0; i < target_provider_set->provider_count; ++i) {
    EXPECT_EQ(environment_provider_set->providers[i],
              target_provider_set->providers[i]);
  }
  for (iree_host_size_t i = 0; i < emitter_provider_set->provider_count; ++i) {
    EXPECT_EQ(environment_provider_set
                  ->providers[target_provider_set->provider_count + i],
              emitter_provider_set->providers[i]);
  }
}

}  // namespace
}  // namespace loom
