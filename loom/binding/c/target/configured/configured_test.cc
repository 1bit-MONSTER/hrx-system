// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loomc/target/configured.h"

#include <memory>

#include "iree/testing/gtest.h"
#include "loom/binding/c/src/target.h"

#ifndef LOOMC_CONFIGURED_HAVE_AMDGPU
#define LOOMC_CONFIGURED_HAVE_AMDGPU 0
#endif  // LOOMC_CONFIGURED_HAVE_AMDGPU
#ifndef LOOMC_CONFIGURED_HAVE_SPIRV
#define LOOMC_CONFIGURED_HAVE_SPIRV 0
#endif  // LOOMC_CONFIGURED_HAVE_SPIRV
#ifndef LOOMC_CONFIGURED_HAVE_VM
#define LOOMC_CONFIGURED_HAVE_VM 0
#endif  // LOOMC_CONFIGURED_HAVE_VM
#ifndef LOOMC_CONFIGURED_HAVE_WASM
#define LOOMC_CONFIGURED_HAVE_WASM 0
#endif  // LOOMC_CONFIGURED_HAVE_WASM
#ifndef LOOMC_CONFIGURED_HAVE_XDNA
#define LOOMC_CONFIGURED_HAVE_XDNA 0
#endif  // LOOMC_CONFIGURED_HAVE_XDNA

namespace {

struct TargetEnvironmentDeleter {
  void operator()(loomc_target_environment_t* environment) const {
    loomc_target_environment_release(environment);
  }
};

TEST(ConfiguredTargetTest, ContainsSelectedEmitters) {
  loomc_target_environment_t* raw_environment = nullptr;
  loomc_status_t status = loomc_target_environment_create_configured(
      loomc_allocator_system(), &raw_environment);
  ASSERT_TRUE(loomc_status_is_ok(status));
  std::unique_ptr<loomc_target_environment_t, TargetEnvironmentDeleter>
      environment(raw_environment);

  const loom_target_environment_t* internal_environment =
      loomc_target_environment_loom_target_environment(environment.get());
  ASSERT_NE(internal_environment, nullptr);
  EXPECT_EQ(loom_target_environment_lookup_emitter(
                internal_environment, IREE_SV("amdgpu-hsaco")) != nullptr,
            static_cast<bool>(LOOMC_CONFIGURED_HAVE_AMDGPU));
  EXPECT_EQ(loom_target_environment_lookup_emitter(internal_environment,
                                                   IREE_SV("spirv")) != nullptr,
            static_cast<bool>(LOOMC_CONFIGURED_HAVE_SPIRV));
  EXPECT_EQ(loom_target_environment_lookup_emitter(internal_environment,
                                                   IREE_SV("vm")) != nullptr,
            static_cast<bool>(LOOMC_CONFIGURED_HAVE_VM));
  EXPECT_EQ(loom_target_environment_lookup_emitter(
                internal_environment, IREE_SV("wasm-binary")) != nullptr,
            static_cast<bool>(LOOMC_CONFIGURED_HAVE_WASM));
  EXPECT_EQ(loom_target_environment_lookup_emitter(internal_environment,
                                                   IREE_SV("xdna")) != nullptr,
            static_cast<bool>(LOOMC_CONFIGURED_HAVE_XDNA));
}

}  // namespace
