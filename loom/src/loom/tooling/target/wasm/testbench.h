// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// WebAssembly execution profile for the shared scenario testbench.

#ifndef LOOM_TOOLING_TARGET_WASM_TESTBENCH_H_
#define LOOM_TOOLING_TARGET_WASM_TESTBENCH_H_

#include "iree/base/api.h"
#include "loom/error/source.h"
#include "loom/target/provider.h"
#include "loom/tooling/execution/execution_provider.h"
#include "loom/tooling/testbench/scenario/executor.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct loom_cleanup_pattern_provider_set_t
    loom_cleanup_pattern_provider_set_t;
typedef struct loom_tooling_config_set_t loom_tooling_config_set_t;

// Complete WebAssembly compiler capabilities contributed to execution tools.
extern const loom_run_execution_provider_t loom_wasm_execution_provider;

// Compiler inputs shared by independently prepared Wasm scenario products.
//
// Each product compiles and instantiates one ordinary function during scenario
// preparation. Trial execution reuses that instance and only transports raw
// scalar payloads and complete physical allocation roots through the
// synchronous host boundary.
typedef struct loom_wasm_testbench_t {
  // Borrowed compiler capabilities, live through the final product prepare.
  const loom_target_environment_t* target_environment;
  // Borrowed cleanup rewrite providers, live through the final prepare.
  const loom_cleanup_pattern_provider_set_t* cleanup_pattern_provider_set;
  // Borrowed admitted source snapshots, live through the final prepare.
  const loom_source_table_resolver_t* sources;
  // Borrowed invocation configuration, live through the final prepare.
  const loom_tooling_config_set_t* config_set;
  // Borrowed diagnostic sink receiving compilation diagnostics.
  loom_diagnostic_sink_t diagnostic_sink;
  // Allocator for compiler scratch and emitted module storage.
  iree_allocator_t host_allocator;
} loom_wasm_testbench_t;

// Initializes an execution profile without compiling or allocating.
void loom_wasm_testbench_initialize(
    const loom_target_environment_t* target_environment,
    const loom_cleanup_pattern_provider_set_t* cleanup_pattern_provider_set,
    iree_allocator_t host_allocator, loom_wasm_testbench_t* out_testbench);

// Binds compilation inputs and returns an eager Wasm execution profile.
// Every prepared product owns an independently compiled and instantiated
// module. Product preparation finishes before trial-local values exist.
loom_testbench_execution_profile_t loom_wasm_testbench_execution_profile(
    void* user_data, const loom_source_table_resolver_t* sources,
    const loom_tooling_config_set_t* config_set,
    loom_diagnostic_sink_t diagnostic_sink);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TOOLING_TARGET_WASM_TESTBENCH_H_
