// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_TOOLING_INPUT_INPUT_H_
#define LOOM_TOOLING_INPUT_INPUT_H_

#include "loom/error/source.h"
#include "loom/format/low_repr.h"
#include "loom/format/text/parser.h"
#include "loom/tooling/io/source.h"
#include "loom/tooling/io/source_path.h"

#ifdef __cplusplus
extern "C" {
#endif

// Shared source selection and per-provider options for multi-input tools.
typedef struct loom_input_options_t {
  // Explicit source format, or empty to select from each filename.
  iree_string_view_t format;
  // Borrowed entries spelled "format:options", at most one per format.
  iree_string_view_list_t provider_options;
  // Display remapping for diagnostics and retained module source names.
  loom_tooling_source_path_options_t source_path_options;
} loom_input_options_t;

// Resolves the option string for |provider|. Malformed entries and duplicate
// entries for this provider fail. Entries for other providers are independent.
iree_status_t loom_input_options_for_provider(iree_string_view_list_t entries,
                                              iree_string_view_t provider,
                                              iree_string_view_t* out_options);

// One source admission request. All views are borrowed until loading returns.
typedef struct loom_input_request_t {
  // Original source bytes, without an intermediate textual IR conversion.
  iree_string_view_t source;
  // Physical source path used for relative include lookup.
  iree_string_view_t path;
  // Optional explicit input format used by tooling provider selection.
  iree_string_view_t format;
  // Provider-owned option spelling, independent of pass/output selection.
  iree_string_view_t options;
  // Diagnostic sink and Loom text parsing environment. Importers use the sink.
  loom_text_parse_options_t parse_options;
  // Descriptor codec used when admitting target Low bytecode.
  loom_low_repr_environment_t low_repr_environment;
  // Display remapping applied to diagnostics and retained module source names.
  loom_tooling_source_path_options_t source_path_options;
} loom_input_request_t;

// Retains the exact bytes admitted by a frontend before its storage expires.
typedef struct loom_input_source_capture_t {
  // Called for the main source and every admitted header; failure aborts load.
  iree_status_t (*fn)(void* user_data, iree_string_view_t filename,
                      iree_string_view_t source);
  // Capture owner borrowed for the duration of loading.
  void* user_data;
} loom_input_source_capture_t;

// Immutable input provider linked by the final tool binary. Providers own
// language options and frontend invocation; modules use ordinary Loom lifetime.
typedef struct loom_input_provider_t {
  // Explicit input-format name, such as "loom" or "cxx".
  iree_string_view_t name;
  // Recognized filename suffixes including the leading dot.
  iree_string_view_list_t suffixes;
  // Loads a module and captures admitted sources. Source rejection emits
  // diagnostics and returns OK with a NULL module. Other failures return a
  // status. A successful module is owned by the caller; context and pool
  // outlive it. Source views from the request expire when this call returns.
  iree_status_t (*load)(const loom_input_request_t* request,
                        loom_input_source_capture_t capture,
                        loom_context_t* context,
                        iree_arena_block_pool_t* block_pool,
                        iree_allocator_t host_allocator,
                        loom_module_t** out_module);
} loom_input_provider_t;

typedef struct loom_input_provider_list_t {
  // Optional providers linked in addition to builtin Loom text and bytecode.
  const loom_input_provider_t* const* values;
  // Number of optional providers.
  iree_host_size_t count;
} loom_input_provider_list_t;

// Builtin Loom text admission, always available without an optional importer.
extern const loom_input_provider_t loom_input_text_provider;

// Builtin serialized Loom module admission. Bytecode is one module, not a
// textual fixture containing directives or split cases.
extern const loom_input_provider_t loom_input_bytecode_provider;

// Selects an explicit format or a filename suffix. Unrecognized extensions
// fail rather than falling through to Loom text. Extensionless input defaults
// to Loom text; an explicit format supports stdin and nonstandard filenames.
iree_status_t loom_input_provider_select(
    loom_input_provider_list_t providers, iree_string_view_t format,
    iree_string_view_t path, const loom_input_provider_t** out_provider);

// Owns a loaded module and its source snapshots. It must stay at a stable
// address until deinitialization. Source resolvers serve the owning module;
// clones must project snapshots through their source-ID correspondence.
typedef struct loom_input_module_t {
  // Owned module, or NULL when source admission did not produce one.
  loom_module_t* module;
  // Logical main-source filename, valid even after source rejection.
  iree_string_view_t filename;
  // Owned snapshots indexed by the module's source IDs, released last.
  loom_tooling_source_storage_t sources;
} loom_input_module_t;

// Loads an input and retains source snapshots through module teardown. Always
// pair with deinitialize, including status failures and source rejection. The
// caller's context and block pool outlive the result. Path remapping cannot
// merge distinct admitted sources into one displayed filename.
iree_status_t loom_input_module_load(const loom_input_provider_t* provider,
                                     const loom_input_request_t* request,
                                     loom_context_t* context,
                                     iree_arena_block_pool_t* block_pool,
                                     iree_allocator_t host_allocator,
                                     loom_input_module_t* out_input);

// Returns a resolver borrowing the retained source table.
loom_source_resolver_t loom_input_module_source_resolver(
    loom_input_module_t* input);

// Releases the module before its retained source snapshots.
void loom_input_module_deinitialize(loom_input_module_t* input);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TOOLING_INPUT_INPUT_H_
