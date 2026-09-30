// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/tooling/compile/preparation.h"

#include "loom/link/linker.h"
#include "loom/target/entry_selection.h"
#include "loom/target/module_specialization.h"

static iree_status_t loom_compile_materialize_roots(
    const loom_compile_request_t* request,
    loom_source_table_projection_t* sources,
    iree_arena_block_pool_t* block_pool, iree_allocator_t allocator,
    loom_module_t** inout_module) {
  loom_module_t* module = *inout_module;
  if (request->selection.roots.count == 0) {
    return iree_ok_status();
  }

  const loom_module_t* const source_modules[] = {module};
  iree_string_view_t module_name = iree_string_view_empty();
  if (module->name_id < module->strings.count) {
    module_name = loom_string_table_get(&module->strings, module->name_id);
  }
  const loom_source_table_resolver_t input_sources = sources->table;
  loom_module_t* linked_module = NULL;
  iree_status_t status = loom_link_materialized_modules(
      source_modules, IREE_ARRAYSIZE(source_modules),
      &(loom_link_options_t){
          .module_name = module_name,
          .root_symbols = request->selection.roots,
          .source_callback = {.fn = loom_source_table_project,
                              .user_data = sources},
      },
      block_pool, allocator, &linked_module);
  if (iree_status_is_ok(status)) {
    loom_module_free(module);
    *inout_module = linked_module;
  } else {
    sources->table = input_sources;
  }
  return status;
}

static iree_status_t loom_compile_specialize_kernel_roots(
    const loom_compile_request_t* request,
    const loom_compile_pipeline_options_t* options,
    iree_arena_block_pool_t* block_pool, iree_allocator_t allocator,
    loom_module_t** inout_module, uint32_t* out_error_count) {
  iree_arena_allocator_t arena;
  iree_arena_initialize(block_pool, &arena);
  // Linking internalizes reachable dependencies and retains every selected
  // function root, preserving the exact product selection without rechecking
  // target-specific op kinds here.
  iree_host_size_t specialization_count = 0;
  for (loom_symbol_id_t symbol_id = 0;
       symbol_id < (*inout_module)->symbols.count; ++symbol_id) {
    const loom_symbol_t* symbol = &(*inout_module)->symbols.entries[symbol_id];
    if (iree_any_bit_set(symbol->flags, LOOM_SYMBOL_FLAG_RETAIN)) {
      ++specialization_count;
    }
  }
  loom_target_specialization_request_t* specializations = NULL;
  iree_status_t status = iree_arena_allocate_array(&arena, specialization_count,
                                                   sizeof(*specializations),
                                                   (void**)&specializations);
  if (iree_status_is_ok(status)) {
    iree_host_size_t specialization_ordinal = 0;
    for (loom_symbol_id_t symbol_id = 0;
         symbol_id < (*inout_module)->symbols.count; ++symbol_id) {
      const loom_symbol_t* symbol =
          &(*inout_module)->symbols.entries[symbol_id];
      if (!iree_any_bit_set(symbol->flags, LOOM_SYMBOL_FLAG_RETAIN)) {
        continue;
      }
      specializations[specialization_ordinal++] =
          (loom_target_specialization_request_t){
              .function_name = loom_string_table_get(&(*inout_module)->strings,
                                                     symbol->name_id),
              .target_profile = request->explicit_target.profile,
          };
    }
    IREE_ASSERT_EQ(specialization_ordinal, specialization_count);
  }
  if (iree_status_is_ok(status)) {
    const loom_target_entry_options_t diagnostic_options = {
        .diagnostic_sink = options->diagnostic_sink,
        .source_resolver = options->source_resolver,
        .max_errors = options->max_errors,
    };
    loom_target_entry_diagnostic_emitter_t diagnostic_emitter;
    loom_target_entry_diagnostic_emitter_initialize(
        *inout_module, &diagnostic_options, LOOM_EMITTER_PASS,
        &diagnostic_emitter);
    status = loom_target_specialize_module(
        options->target_environment,
        (loom_target_specialization_request_list_t){
            .values = specializations,
            .count = specialization_count,
        },
        (loom_target_declaration_binding_list_t){0},
        loom_target_entry_emitter(&diagnostic_emitter), block_pool, allocator,
        inout_module, out_error_count);
  }
  iree_arena_deinitialize(&arena);
  return status;
}

iree_status_t loom_compile_materialize_request(
    const loom_compile_request_t* request,
    const loom_compile_pipeline_options_t* options,
    loom_source_table_projection_t* sources,
    iree_arena_block_pool_t* block_pool, iree_allocator_t allocator,
    loom_module_t** inout_module, uint32_t* out_error_count) {
  *out_error_count = 0;
  IREE_RETURN_IF_ERROR(loom_compile_materialize_roots(
      request, sources, block_pool, allocator, inout_module));
  if (request->selection.product == LOOM_COMPILE_PRODUCT_KERNEL &&
      request->explicit_target.profile != NULL) {
    IREE_RETURN_IF_ERROR(loom_compile_specialize_kernel_roots(
        request, options, block_pool, allocator, inout_module,
        out_error_count));
    // Standalone target specialization is an exact module clone: source IDs
    // remain unchanged while ownership moves to its replacement.
    sources->table.module = *inout_module;
  }
  if (*out_error_count != 0) {
    return iree_ok_status();
  }
  return iree_ok_status();
}

iree_status_t loom_compile_run_request_pipeline(
    const loom_compile_request_t* request, loom_module_t* module,
    const loom_compile_pipeline_options_t* options,
    iree_arena_block_pool_t* block_pool,
    loom_compile_pipeline_result_t* out_result) {
  memset(out_result, 0, sizeof(*out_result));
  loom_compile_pipeline_options_t pipeline_options = *options;

  // Selected roots are public or retained by module linking. Specialization
  // owns their transitive callees and carries facts directly through emission,
  // without projecting target definitions back into the authored module.
  iree_arena_allocator_t arena;
  iree_arena_initialize(block_pool, &arena);
  iree_status_t status = iree_ok_status();
  if (request->selection.product == LOOM_COMPILE_PRODUCT_MODULE &&
      request->explicit_target.profile != NULL) {
    loom_target_specialization_request_t* specializations = NULL;
    status = iree_arena_allocate_array(&arena, module->symbols.count,
                                       sizeof(*specializations),
                                       (void**)&specializations);
    if (iree_status_is_ok(status)) {
      iree_host_size_t count = 0;
      for (iree_host_size_t i = 0; i < module->symbols.count; ++i) {
        const loom_symbol_t* symbol = &module->symbols.entries[i];
        const loom_func_like_t function =
            loom_func_like_cast(module, symbol->defining_op);
        if (loom_func_like_body(function) == NULL ||
            (loom_func_like_is_module_internal(function) &&
             !iree_any_bit_set(symbol->flags, LOOM_SYMBOL_FLAG_RETAIN))) {
          continue;
        }
        specializations[count++] = (loom_target_specialization_request_t){
            .function_name =
                loom_string_table_get(&module->strings, symbol->name_id),
            .target_profile = request->explicit_target.profile,
        };
      }
      pipeline_options.target_specializations =
          (loom_target_specialization_request_list_t){specializations, count};
    }
  }
  if (iree_status_is_ok(status)) {
    status = loom_compile_run_pipeline(module, &pipeline_options, block_pool,
                                       out_result);
  }
  iree_arena_deinitialize(&arena);
  return status;
}
