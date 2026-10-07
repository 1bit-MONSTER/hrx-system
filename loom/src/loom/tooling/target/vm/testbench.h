// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_TOOLING_TARGET_VM_TESTBENCH_H_
#define LOOM_TOOLING_TARGET_VM_TESTBENCH_H_

#include "iree/vm/buffer.h"
#include "iree/vm/invocation.h"
#include "iree/vm/process.h"
#include "loom/error/source.h"
#include "loom/target/provider.h"
#include "loom/tooling/execution/execution_provider.h"
#include "loom/tooling/testbench/invocation.h"
#include "loom/tooling/testbench/scenario/executor.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct loom_cleanup_pattern_provider_set_t
    loom_cleanup_pattern_provider_set_t;

// Complete VM compiler capabilities contributed to execution tools.
extern const loom_run_execution_provider_t loom_vm_execution_provider;

// Executes semantic functions through the VM. The check.case provider compiles
// all selected function roots on its first call and reuses one process. The
// check.scenario profile instead uses this instance as compiler configuration
// and eagerly creates an independent bytecode module and process per prepared
// target or oracle product. The authored functions need no target binding.
// Buffer arguments and results share storage with testbench HAL bindings.
// Arguments require coherent persistent host mappings; results retain the VM
// storage until the final binding or alias is released.
typedef struct loom_vm_testbench_t {
  // Borrowed compiler capabilities, live through deinitialization.
  const loom_target_environment_t* target_environment;
  // Borrowed cleanup rewrite providers, live through deinitialization.
  const loom_cleanup_pattern_provider_set_t* cleanup_pattern_provider_set;
  // Borrowed selected cases identifying the functions crossing the host ABI.
  loom_testbench_case_plan_list_t cases;
  // Borrowed admitted source snapshots, live through the final invocation.
  const loom_source_table_resolver_t* sources;
  // Borrowed invocation configuration, live through the final function call.
  const loom_tooling_config_set_t* config_set;
  // Borrowed diagnostic sink receiving compilation diagnostics.
  loom_diagnostic_sink_t diagnostic_sink;
  // Allocator for bytecode and runtime objects.
  iree_allocator_t host_allocator;
  // Whether compilation semantically rejected the selected source module.
  bool compile_rejected;
  // Stable compilation stage that rejected the source module.
  iree_string_view_t compile_failure_stage;
  // Stable diagnostic or fallback rejection identifier.
  iree_string_view_t compile_failure_kind;
  // Static human-facing summary of the compilation rejection.
  iree_string_view_t compile_failure_message;
  // Owned process, or NULL until the first function call is prepared.
  iree_vm_process_t* process;
  // Owned reusable execution storage, never shared by concurrent calls.
  iree_vm_invocation_t* invocation;
  // Owned reusable argument variants; also the base of the combined IO slab.
  iree_vm_variant_t* arguments;
  // Result variants within the IO slab, sized from the immutable case plan.
  iree_vm_variant_t* results;
  // Retained imported buffer arguments used to trace returned references.
  iree_vm_buffer_t** argument_buffers;
  // Borrowed Core descriptors whose provider lives with the linked VM runtime.
  iree_vm_ref_types_t ref_types;
} loom_vm_testbench_t;

// Initializes a lazy function provider without compiling or allocating.
void loom_vm_testbench_initialize(
    const loom_target_environment_t* target_environment,
    const loom_cleanup_pattern_provider_set_t* cleanup_pattern_provider_set,
    iree_allocator_t host_allocator, loom_vm_testbench_t* out_testbench);

// Releases runtime objects. Safe for a zero-initialized or failed provider.
void loom_vm_testbench_deinitialize(loom_vm_testbench_t* testbench);

// Binds the runner-selected cases and returns a borrowed function-call
// callback. |user_data| points to an initialized loom_vm_testbench_t. The case
// list, source table and optional config set remain live through the final
// call; deinitialization does not access them. Snapshots follow the compiler
// copy through its source-ID map. Configuration specializes that private copy.
loom_testbench_invocation_provider_t loom_vm_testbench_invocation_provider(
    void* user_data, loom_testbench_case_plan_list_t cases,
    const loom_source_table_resolver_t* sources,
    const loom_tooling_config_set_t* config_set);

// Binds compilation inputs and returns an eager VM execution profile. Every
// prepared product owns an independently compiled bytecode module and process.
// Product preparation finishes before any trial-local values are materialized;
// product execution only marshals batches through that prepared process.
loom_testbench_execution_profile_t loom_vm_testbench_execution_profile(
    void* user_data, const loom_source_table_resolver_t* sources,
    const loom_tooling_config_set_t* config_set,
    loom_diagnostic_sink_t diagnostic_sink);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TOOLING_TARGET_VM_TESTBENCH_H_
