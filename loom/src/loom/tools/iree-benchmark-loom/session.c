// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/tools/iree-benchmark-loom/session.h"

iree_status_t iree_benchmark_loom_session_initialize(
    const iree_benchmark_loom_configuration_t* configuration,
    iree_allocator_t host_allocator, loom_run_session_t* out_session) {
  IREE_ASSERT_ARGUMENT(configuration);
  IREE_ASSERT_ARGUMENT(out_session);
  loom_run_session_options_t session_options = {0};
  loom_run_session_options_initialize(&session_options);
  session_options.host_allocator = host_allocator;
  session_options.input_providers = configuration->input_providers;
  session_options.target_environment = configuration->target_environment;
  session_options.cleanup_pattern_provider_set =
      configuration->cleanup_pattern_provider_set;
  return loom_run_session_initialize(&session_options, out_session);
}
