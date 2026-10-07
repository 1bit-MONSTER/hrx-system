// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/tooling/execution/execution_provider.h"

static iree_status_t loom_run_execution_environment_append_execution_backends(
    loom_run_execution_environment_t* environment,
    const loom_run_execution_provider_t* provider) {
  if (environment->execution_backend_count + provider->execution_backend_count >
      IREE_ARRAYSIZE(environment->execution_backends)) {
    return iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                            "loom execution backend capacity exceeded");
  }
  for (iree_host_size_t i = 0; i < provider->execution_backend_count; ++i) {
    environment->execution_backends[environment->execution_backend_count++] =
        provider->execution_backends[i];
  }
  return iree_ok_status();
}

iree_status_t loom_run_execution_environment_initialize(
    const loom_run_execution_provider_set_t* provider_set,
    loom_run_execution_environment_t* out_environment) {
  *out_environment = (loom_run_execution_environment_t){
      .provider_set = provider_set,
  };
  loom_target_provider_set_storage_initialize(
      &out_environment->compiler_provider_storage);

  for (iree_host_size_t i = 0; i < provider_set->provider_count; ++i) {
    const loom_run_execution_provider_t* provider = provider_set->providers[i];
    if (provider->compiler_provider_set != NULL) {
      IREE_RETURN_IF_ERROR(loom_target_provider_set_storage_append_set(
          &out_environment->compiler_provider_storage,
          provider->compiler_provider_set));
    }
    IREE_RETURN_IF_ERROR(
        loom_run_execution_environment_append_execution_backends(
            out_environment, provider));
  }
  IREE_RETURN_IF_ERROR(loom_target_environment_initialize(
      &out_environment->compiler_provider_storage.provider_set,
      &out_environment->target_environment));
  loom_run_execution_backend_registry_initialize_from_entries(
      out_environment->execution_backends,
      out_environment->execution_backend_count,
      &out_environment->execution_backend_registry);
  return iree_ok_status();
}

void loom_run_execution_environment_deinitialize(
    loom_run_execution_environment_t* environment) {
  if (environment == NULL) {
    return;
  }
  *environment = (loom_run_execution_environment_t){0};
}

const loom_target_environment_t*
loom_run_execution_environment_target_environment(
    const loom_run_execution_environment_t* environment) {
  return &environment->target_environment;
}

const loom_run_execution_backend_registry_t*
loom_run_execution_environment_execution_backend_registry(
    const loom_run_execution_environment_t* environment) {
  return &environment->execution_backend_registry;
}
