// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/tooling/compile/configured.h"

#include "iree/base/threading/call_once.h"
#include "loom/target/configured/compiler_provider_set.h"
#include "loom/target/configured/provider_set.h"
#include "loom/transforms/cleanup/configured.h"

typedef struct loom_tooling_configured_compile_storage_t {
  // Configured targets and emitters retained for the environment lifetime.
  loom_target_provider_set_storage_t target_provider_storage;
  // Composed compiler target environment.
  loom_target_environment_t target_environment;
  // Public borrowed view over the configured compiler providers.
  loom_tooling_compile_environment_t environment;
} loom_tooling_configured_compile_storage_t;

static loom_tooling_configured_compile_storage_t configured_compile_storage;
static iree_once_flag configured_compile_once = IREE_ONCE_FLAG_INIT;

static iree_status_t loom_tooling_configured_compile_initialize_storage(void) {
  loom_target_provider_set_storage_initialize(
      &configured_compile_storage.target_provider_storage);
  IREE_RETURN_IF_ERROR(loom_target_provider_set_storage_append_set(
      &configured_compile_storage.target_provider_storage,
      loom_configured_target_provider_set()));
  IREE_RETURN_IF_ERROR(loom_target_provider_set_storage_append_set(
      &configured_compile_storage.target_provider_storage,
      loom_configured_emitter_provider_set()));
  IREE_RETURN_IF_ERROR(loom_target_environment_initialize(
      &configured_compile_storage.target_provider_storage.provider_set,
      &configured_compile_storage.target_environment));
  configured_compile_storage.environment = (loom_tooling_compile_environment_t){
      .target_environment = &configured_compile_storage.target_environment,
      .cleanup_pattern_provider_set =
          loom_cleanup_configured_pattern_provider_set(),
  };
  return iree_ok_status();
}

static void loom_tooling_configured_compile_initialize_once(void) {
  IREE_CHECK_OK(loom_tooling_configured_compile_initialize_storage());
}

const loom_tooling_compile_environment_t*
loom_tooling_configured_compile_environment(void) {
  iree_call_once(&configured_compile_once,
                 loom_tooling_configured_compile_initialize_once);
  return &configured_compile_storage.environment;
}
