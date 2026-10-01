// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Offline compiler qualification, independent of execution resources.

#ifndef LOOM_TOOLS_LOOM_CHECK_COMPILE_H_
#define LOOM_TOOLS_LOOM_CHECK_COMPILE_H_

#include "loom/tooling/compile/configured.h"
#include "loom/tooling/config/config.h"
#include "loom/tools/loom-check/execute.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct loom_check_compile_options_t {
  // Family-qualified compiler profile; empty when qualification is disabled.
  iree_string_view_t target;
  // Configured offline providers, borrowed for the entire check invocation.
  const loom_tooling_compile_environment_t* environment;
  // Compile-time bindings applied before resolving roots and specializing.
  const loom_tooling_config_set_t* config_set;
  // Instrumentation applied to the independently compiled artifact.
  loom_sanitizer_options_t sanitizer;
} loom_check_compile_options_t;

// Compiles one admitted source case through request materialization and the
// final artifact producer. Success requires a nonempty artifact or exactly
// matched diagnostic annotations. RUN goldens, XFAIL, and external execution
// requirements do not describe this independent compile outcome. No device is
// opened and no artifact is loaded or executed.
iree_status_t loom_check_execute_compile(
    const loom_test_case_t* test_case, iree_host_size_t case_index,
    loom_check_file_report_t* report, iree_string_view_t filename,
    const loom_input_request_t* input_request,
    const loom_check_compile_options_t* options,
    const loom_check_environment_t* environment, loom_context_t* context,
    iree_arena_block_pool_t* block_pool, iree_allocator_t allocator,
    loom_check_result_t* result);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TOOLS_LOOM_CHECK_COMPILE_H_
