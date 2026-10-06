// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Shared execution session and parsed-module lifecycle for Loom tools.

#ifndef LOOM_TOOLING_EXECUTION_SESSION_H_
#define LOOM_TOOLING_EXECUTION_SESSION_H_

#include "iree/base/api.h"
#include "iree/base/internal/arena.h"
#include "loom/error/source.h"
#include "loom/ir/context.h"
#include "loom/ir/module.h"
#include "loom/target/low_descriptor_registry.h"
#include "loom/tooling/input/input.h"
#include "loom/tooling/io/source.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct loom_cleanup_pattern_provider_set_t
    loom_cleanup_pattern_provider_set_t;

// Registers the dialect surface selected by an execution environment.
typedef iree_status_t (*loom_run_register_context_fn_t)(
    void* user_data, loom_context_t* context);

typedef struct loom_run_register_context_callback_t {
  // Function that registers dialects and encoding families.
  loom_run_register_context_fn_t fn;
  // Caller-owned payload forwarded to |fn|.
  void* user_data;
} loom_run_register_context_callback_t;

// Initializes the target-low descriptor registry linked into this runner.
typedef iree_status_t (*loom_run_initialize_low_descriptor_registry_fn_t)(
    void* user_data, loom_target_low_descriptor_registry_t* out_registry);

typedef struct loom_run_initialize_low_descriptor_registry_callback_t {
  // Function that initializes the selected target-low descriptor package.
  loom_run_initialize_low_descriptor_registry_fn_t fn;
  // Caller-owned payload forwarded to |fn|.
  void* user_data;
} loom_run_initialize_low_descriptor_registry_callback_t;

typedef struct loom_run_session_options_t {
  // Borrowed optional input providers linked by the final application.
  loom_input_provider_list_t input_providers;
  // Host allocator used for session-owned runtime state.
  iree_allocator_t host_allocator;
  // Total bytes retained per transient parser/compiler arena block.
  iree_host_size_t block_pool_block_size;
  // Dialect and encoding registration callback.
  loom_run_register_context_callback_t register_context;
  // Descriptor registry initialization callback.
  loom_run_initialize_low_descriptor_registry_callback_t
      initialize_low_descriptor_registry;
  // Cleanup rewrite providers linked into this runner.
  const loom_cleanup_pattern_provider_set_t* cleanup_pattern_provider_set;
} loom_run_session_options_t;

typedef struct loom_run_session_t {
  // Borrowed optional input providers, live through all module admissions.
  loom_input_provider_list_t input_providers;
  // Host allocator used for session-owned runtime state.
  iree_allocator_t host_allocator;
  // Transient block pool reused across modules and candidates.
  iree_arena_block_pool_t block_pool;
  // Finalized context containing the linked dialect surface.
  loom_context_t context;
  // Descriptor registry selected by the runner environment.
  loom_target_low_descriptor_registry_t low_descriptor_registry;
  // Borrowed cleanup rewrite providers selected by the runner environment.
  const loom_cleanup_pattern_provider_set_t* cleanup_pattern_provider_set;
  // True when |block_pool| has been initialized.
  bool block_pool_initialized;
  // True when |context| has been initialized and must be deinitialized.
  bool context_initialized;
} loom_run_session_t;

// Initializes options with the default allocator and block-pool size.
void loom_run_session_options_initialize(
    loom_run_session_options_t* out_options);

// Initializes a reusable run/check/tune session.
iree_status_t loom_run_session_initialize(
    const loom_run_session_options_t* options, loom_run_session_t* out_session);

// Releases all resources owned by |session|.
void loom_run_session_deinitialize(loom_run_session_t* session);

// Returns the finalized context owned by |session|.
loom_context_t* loom_run_session_context(loom_run_session_t* session);

// Returns the transient block pool owned by |session|.
iree_arena_block_pool_t* loom_run_session_block_pool(
    loom_run_session_t* session);

// Returns the target-low descriptor registry owned by |session|.
const loom_target_low_descriptor_registry_t*
loom_run_session_low_descriptor_registry(const loom_run_session_t* session);

// Returns the cleanup rewrite providers selected by the runner environment.
const loom_cleanup_pattern_provider_set_t*
loom_run_session_cleanup_pattern_provider_set(
    const loom_run_session_t* session);

typedef struct loom_run_module_parse_options_t {
  // Source format, provider options, and diagnostic filename remapping.
  loom_input_options_t input;
  // Physical input path used for include lookup and default diagnostics.
  iree_string_view_t filename;
  // Input bytes. Text is parsed directly; bytecode is detected by file magic.
  // Borrowed for the parse call; captured text snapshots are owned by the
  // result.
  iree_string_view_t source;
  // Diagnostic sink used by the text parser or bytecode reader.
  loom_diagnostic_sink_t diagnostic_sink;
  // Maximum number of parse diagnostics before stopping. Zero means no limit.
  uint32_t max_errors;
} loom_run_module_parse_options_t;

typedef struct loom_run_module_t {
  // Parsed module owned by this object.
  loom_module_t* module;
  // Input path borrowed from the caller, which must outlive this object.
  // Diagnostic source filenames are owned separately in sources.
  iree_string_view_t filename;
  // Owned source snapshots for text inputs and linked dependencies.
  loom_tooling_source_storage_t sources;
} loom_run_module_t;

// Initializes parse options with stderr diagnostics and a small error cap.
void loom_run_module_parse_options_initialize(
    loom_run_module_parse_options_t* out_options);

// Imports source or reads bytecode into a module owned by |out_module|.
iree_status_t loom_run_module_parse(
    loom_run_session_t* session, const loom_run_module_parse_options_t* options,
    loom_run_module_t* out_module);

// Clones selected roots and their dependencies, preserving diagnostic sources.
// An empty |root_symbols| clones the entire module. The result owns its IR and
// source snapshots; its borrowed filename and session must outlive the result.
iree_status_t loom_run_module_clone(loom_run_session_t* session,
                                    const loom_run_module_t* source,
                                    iree_string_view_list_t root_symbols,
                                    loom_run_module_t* out_module);

// Releases the parsed module owned by |run_module|.
void loom_run_module_deinitialize(loom_run_module_t* run_module);

// Returns a source resolver for diagnostics against |run_module|.
loom_source_resolver_t loom_run_module_source_resolver(
    const loom_run_module_t* run_module);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TOOLING_EXECUTION_SESSION_H_
