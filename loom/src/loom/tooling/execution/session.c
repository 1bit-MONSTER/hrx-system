// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/tooling/execution/session.h"

#include <string.h>

#include "loom/codegen/low/repr.h"
#include "loom/codegen/low/text_asm.h"
#include "loom/error/diagnostic.h"
#include "loom/error/source.h"
#include "loom/format/bytecode/format.h"
#include "loom/format/bytecode/reader.h"
#include "loom/format/text/parser.h"
#include "loom/ir/module.h"
#include "loom/link/linker.h"

enum {
  LOOM_RUN_DEFAULT_BLOCK_POOL_BLOCK_SIZE = 128 * 1024,
  LOOM_RUN_DEFAULT_MAX_PARSE_ERRORS = 20,
};

void loom_run_session_options_initialize(
    loom_run_session_options_t* out_options) {
  *out_options = (loom_run_session_options_t){
      .host_allocator = iree_allocator_system(),
      .block_pool_block_size = LOOM_RUN_DEFAULT_BLOCK_POOL_BLOCK_SIZE,
  };
}

iree_status_t loom_run_session_initialize(
    const loom_run_session_options_t* options,
    loom_run_session_t* out_session) {
  *out_session = (loom_run_session_t){
      .host_allocator = options->host_allocator,
      .input_providers = options->input_providers,
      .cleanup_pattern_provider_set = options->cleanup_pattern_provider_set,
  };

  const iree_host_size_t block_pool_block_size =
      options->block_pool_block_size == 0
          ? LOOM_RUN_DEFAULT_BLOCK_POOL_BLOCK_SIZE
          : options->block_pool_block_size;
  if (IREE_UNLIKELY(block_pool_block_size < sizeof(iree_arena_block_t))) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "session block pool size is too small");
  }
  if (IREE_UNLIKELY(
          !iree_arena_block_pool_is_valid_total_size(block_pool_block_size))) {
    return iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                            "session block pool size is too large");
  }
  iree_arena_block_pool_initialize(
      block_pool_block_size, options->host_allocator, &out_session->block_pool);
  out_session->block_pool_initialized = true;

  iree_status_t status = options->initialize_low_descriptor_registry.fn(
      options->initialize_low_descriptor_registry.user_data,
      &out_session->low_descriptor_registry);
  if (iree_status_is_ok(status)) {
    loom_context_initialize(options->host_allocator, &out_session->context);
    out_session->context_initialized = true;
    status = options->register_context.fn(options->register_context.user_data,
                                          &out_session->context);
  }
  if (iree_status_is_ok(status)) {
    status = loom_context_finalize(&out_session->context);
  }
  if (!iree_status_is_ok(status)) {
    loom_run_session_deinitialize(out_session);
  }
  return status;
}

void loom_run_session_deinitialize(loom_run_session_t* session) {
  if (session == NULL) {
    return;
  }
  if (session->context_initialized) {
    loom_context_deinitialize(&session->context);
  }
  if (session->block_pool_initialized) {
    iree_arena_block_pool_deinitialize(&session->block_pool);
  }
  *session = (loom_run_session_t){0};
}

loom_context_t* loom_run_session_context(loom_run_session_t* session) {
  return &session->context;
}

iree_arena_block_pool_t* loom_run_session_block_pool(
    loom_run_session_t* session) {
  return &session->block_pool;
}

const loom_target_low_descriptor_registry_t*
loom_run_session_low_descriptor_registry(const loom_run_session_t* session) {
  return &session->low_descriptor_registry;
}

const loom_cleanup_pattern_provider_set_t*
loom_run_session_cleanup_pattern_provider_set(
    const loom_run_session_t* session) {
  return session->cleanup_pattern_provider_set;
}

void loom_run_module_parse_options_initialize(
    loom_run_module_parse_options_t* out_options) {
  *out_options = (loom_run_module_parse_options_t){
      .diagnostic_sink = {.fn = loom_diagnostic_stderr_sink},
      .max_errors = LOOM_RUN_DEFAULT_MAX_PARSE_ERRORS,
  };
}

static bool loom_run_module_input_is_bytecode(iree_string_view_t source) {
  return source.size >= LOOM_BYTECODE_MAGIC_LENGTH &&
         memcmp(source.data, LOOM_BYTECODE_MAGIC, LOOM_BYTECODE_MAGIC_LENGTH) ==
             0;
}

static iree_status_t loom_run_module_import_source(
    loom_run_session_t* session, const loom_run_module_parse_options_t* options,
    loom_run_module_t* out_module) {
  const loom_input_provider_t* provider = NULL;
  IREE_RETURN_IF_ERROR(loom_input_provider_select(
      session->input_providers, options->input.format, options->filename,
      &provider));
  loom_input_request_t request = {
      .source = options->source,
      .path = options->filename,
      .format = provider->name,
      .parse_options = {.diagnostic_sink = options->diagnostic_sink,
                        .max_errors = options->max_errors},
      .source_path_options = options->input.source_path_options,
  };
  IREE_RETURN_IF_ERROR(loom_input_options_for_provider(
      options->input.provider_options, provider->name, &request.options));
  loom_low_descriptor_text_asm_environment_initialize(
      &session->low_descriptor_registry.registry,
      &request.parse_options.low_asm_environment);
  loom_input_module_t input = {0};
  iree_status_t status = loom_input_module_load(
      provider, &request, &session->context, &session->block_pool,
      session->host_allocator, &input);
  if (iree_status_is_ok(status) && input.module == NULL) {
    status = iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                              "input module has source errors");
  }
  if (iree_status_is_ok(status)) {
    out_module->module = input.module;
    input.module = NULL;
    loom_tooling_source_storage_deinitialize(&out_module->sources);
    out_module->sources = input.sources;
    input.sources = (loom_tooling_source_storage_t){0};
  }
  loom_input_module_deinitialize(&input);
  return status;
}

static iree_status_t loom_run_module_read_bytecode(
    loom_run_session_t* session, const loom_run_module_parse_options_t* options,
    loom_run_module_t* out_module) {
  const iree_const_byte_span_t bytecode = iree_make_const_byte_span(
      options->source.data, (iree_host_size_t)options->source.size);
  loom_bytecode_read_options_t read_options = {
      .diagnostic_sink = options->diagnostic_sink,
  };
  loom_low_repr_environment_initialize(
      &session->low_descriptor_registry.registry,
      &read_options.low_repr_environment);
  loom_bytecode_read_result_t read_result = {0};
  IREE_RETURN_IF_ERROR(loom_bytecode_read_module(
      bytecode, options->filename, &session->context, &session->block_pool,
      &read_options, &read_result, &out_module->module,
      session->host_allocator));
  if (read_result.error_count > 0 || out_module->module == NULL) {
    return iree_make_status(
        IREE_STATUS_INVALID_ARGUMENT, "failed to read bytecode input '%.*s'",
        (int)options->filename.size, options->filename.data);
  }
  return iree_ok_status();
}

iree_status_t loom_run_module_parse(
    loom_run_session_t* session, const loom_run_module_parse_options_t* options,
    loom_run_module_t* out_module) {
  *out_module = (loom_run_module_t){
      .filename = options->filename,
  };

  loom_tooling_source_storage_initialize(&session->block_pool,
                                         &out_module->sources);

  iree_status_t status =
      loom_run_module_input_is_bytecode(options->source)
          ? loom_run_module_read_bytecode(session, options, out_module)
          : loom_run_module_import_source(session, options, out_module);
  if (!iree_status_is_ok(status)) {
    loom_run_module_deinitialize(out_module);
  }
  return status;
}

typedef struct loom_run_module_clone_sources_t {
  // Original snapshots borrowed for the duration of linking.
  const loom_source_table_resolver_t* source;
  // Destination storage owned by the cloned module.
  loom_tooling_source_storage_t* target;
} loom_run_module_clone_sources_t;

static iree_status_t loom_run_module_clone_sources(
    void* user_data, const loom_module_t* source_module,
    const loom_module_t* target_module,
    const loom_source_id_t* target_sources) {
  const loom_run_module_clone_sources_t* sources = user_data;
  return loom_tooling_source_storage_project(sources->target, target_module,
                                             sources->source, target_sources);
}

iree_status_t loom_run_module_clone(loom_run_session_t* session,
                                    const loom_run_module_t* source,
                                    iree_string_view_list_t root_symbols,
                                    loom_run_module_t* out_module) {
  *out_module = (loom_run_module_t){.filename = source->filename};
  loom_tooling_source_storage_initialize(&session->block_pool,
                                         &out_module->sources);
  const loom_module_t* const source_modules[] = {source->module};
  loom_run_module_clone_sources_t sources = {
      .source = &source->sources.table,
      .target = &out_module->sources,
  };
  const loom_link_options_t options = {
      .module_name = source->module->name_id < source->module->strings.count
                         ? loom_string_table_get(&source->module->strings,
                                                 source->module->name_id)
                         : iree_string_view_empty(),
      .root_symbols = root_symbols,
      .source_callback = {.fn = loom_run_module_clone_sources,
                          .user_data = &sources},
  };
  iree_status_t status = loom_link_materialized_modules(
      source_modules, IREE_ARRAYSIZE(source_modules), &options,
      &session->block_pool, session->host_allocator, &out_module->module);
  if (!iree_status_is_ok(status)) {
    loom_run_module_deinitialize(out_module);
  }
  return status;
}

void loom_run_module_deinitialize(loom_run_module_t* run_module) {
  if (run_module == NULL) {
    return;
  }
  loom_module_free(run_module->module);
  loom_tooling_source_storage_deinitialize(&run_module->sources);
  *run_module = (loom_run_module_t){0};
}

loom_source_resolver_t loom_run_module_source_resolver(
    const loom_run_module_t* run_module) {
  if (run_module == NULL) {
    return (loom_source_resolver_t){0};
  }
  return loom_tooling_source_storage_resolver(&run_module->sources);
}
