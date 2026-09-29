// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// WebAssembly execution for host testbench products.

#ifndef LOOM_TOOLING_TARGET_WASM_TESTBENCH_H_
#define LOOM_TOOLING_TARGET_WASM_TESTBENCH_H_

#include "iree/base/api.h"
#include "loom/error/source.h"
#include "loom/target/provider.h"
#include "loom/tooling/testbench/scenario/executor.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct loom_cleanup_pattern_provider_set_t
    loom_cleanup_pattern_provider_set_t;
typedef struct loom_wasm_evaluator_t loom_wasm_evaluator_t;
typedef struct loom_tooling_config_set_t loom_tooling_config_set_t;

// Shared compiler and evaluator state for Wasm testbench products. Each
// prepared product owns an ordinary module instance in |evaluator| while the
// Node.js process is shared across all products prepared by this testbench.
typedef struct loom_wasm_testbench_t {
  // Borrowed compiler capabilities, live through deinitialization.
  const loom_target_environment_t* target_environment;
  // Borrowed cleanup rewrite providers, live through deinitialization.
  const loom_cleanup_pattern_provider_set_t* cleanup_pattern_provider_set;
  // Borrowed executable path used when the evaluator is started lazily.
  iree_string_view_t node_executable;
  // Borrowed target selected by the runner for the active module.
  const loom_target_profile_t* target_profile;
  // Borrowed admitted source snapshots, live through product preparation.
  const loom_source_table_resolver_t* sources;
  // Borrowed invocation configuration, live through product preparation.
  const loom_tooling_config_set_t* config_set;
  // Persistent evaluator created by the first prepared Wasm product.
  loom_wasm_evaluator_t* evaluator;
  // Allocator owning the evaluator process and protocol buffers.
  iree_allocator_t host_allocator;
} loom_wasm_testbench_t;

// Initializes a lazy Wasm testbench without starting an evaluator process.
void loom_wasm_testbench_initialize(
    const loom_target_environment_t* target_environment,
    const loom_cleanup_pattern_provider_set_t* cleanup_pattern_provider_set,
    iree_string_view_t node_executable, iree_allocator_t host_allocator,
    loom_wasm_testbench_t* out_testbench);

// Stops the shared evaluator and releases its resources. Returns evaluator
// transport, child-process, and deferred product-release failures.
iree_status_t loom_wasm_testbench_deinitialize(
    loom_wasm_testbench_t* testbench);

// Binds compiler inputs and one runner-selected Wasm target profile. Product
// preparation compiles the selected semantic function into an ordinary Wasm
// module and instantiates it in the shared evaluator.
loom_testbench_execution_profile_t loom_wasm_testbench_execution_profile(
    void* user_data, const loom_target_profile_t* target_profile,
    const loom_source_table_resolver_t* sources,
    const loom_tooling_config_set_t* config_set);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TOOLING_TARGET_WASM_TESTBENCH_H_
