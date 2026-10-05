// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOMC_PASS_TRACE_STORAGE_H_
#define LOOMC_PASS_TRACE_STORAGE_H_

#include "loom/ir/function_version.h"
#include "loom/pass/trace.h"
#include "loomc/pass_trace.h"
#include "visibility.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct loomc_target_pass_environment_t loomc_target_pass_environment_t;

typedef struct loomc_pass_trace_state_t {
  // Borrowed public options owning callback state and filter strings.
  const loomc_pass_trace_options_t* public_options;

  // True when the public destination returned a non-OK status.
  bool sink_failed;

  // Core callback stream adapting public status and string types.
  loom_output_stream_t stream;

  // Core trace options borrowing the public descriptor for this invocation.
  loom_pass_trace_options_t options;

  // Live function versions whose target facts survive trace projection.
  const loom_function_version_list_t* function_versions;

  // Block pool used for short-lived projected snapshots.
  iree_arena_block_pool_t* block_pool;

  // Core trace state passed to the interpreter.
  loom_pass_trace_t trace;
} loomc_pass_trace_state_t;

// Validates one public pass trace descriptor.
LOOMC_API_PRIVATE loomc_status_t
loomc_pass_trace_options_validate(const loomc_pass_trace_options_t* options);

// Initializes one invocation-local core trace over public callbacks.
LOOMC_API_PRIVATE void loomc_pass_trace_state_initialize(
    const loomc_pass_trace_options_t* options, iree_string_view_t stage,
    const loomc_target_pass_environment_t* target_environment,
    const loom_function_version_list_t* function_versions,
    iree_arena_block_pool_t* block_pool, loomc_pass_trace_state_t* out_state);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOMC_PASS_TRACE_STORAGE_H_
