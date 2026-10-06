// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/binding/c/target/provider_set.h"
#include "loom/target/arch/spirv/compiler_provider.h"
#include "loomc/target/spirv/base.h"
#include "target.h"

const loom_target_provider_set_t* loomc_spirv_provider_set(void) {
  return &loom_spirv_compiler_provider_set;
}

loomc_status_t loomc_target_environment_create_spirv(
    loomc_allocator_t allocator,
    loomc_target_environment_t** out_target_environment) {
  return loomc_target_environment_create_from_provider_set(
      loomc_spirv_provider_set(), allocator, out_target_environment);
}
