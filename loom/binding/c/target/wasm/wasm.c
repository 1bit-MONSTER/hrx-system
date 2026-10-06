// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loomc/target/wasm.h"

#include "loom/target/emit/wasm/module_compiler.h"
#include "target.h"

loomc_status_t loomc_target_environment_create_wasm(
    loomc_allocator_t allocator,
    loomc_target_environment_t** out_target_environment) {
  return loomc_target_environment_create_from_provider_set(
      &loom_wasm_compiler_provider_set, allocator, out_target_environment);
}
