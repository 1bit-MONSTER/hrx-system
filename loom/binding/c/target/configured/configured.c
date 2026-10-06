// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loomc/target/configured.h"

#include "loom/target/configured/compiler_provider_set.h"
#include "target.h"

loomc_status_t loomc_target_environment_create_configured(
    loomc_allocator_t allocator,
    loomc_target_environment_t** out_target_environment) {
  return loomc_target_environment_create_from_provider_set(
      loom_configured_compiler_provider_set(), allocator,
      out_target_environment);
}
