// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/import/cxx/tooling/loomc_input.h"

#include "loom/import/cxx/tooling/input.h"
#include "loomc/iree.h"

IREE_STATIC_ASSERT_ENUM_EQ(LOOM_CXX_DATA_MODEL_LP64, LOOMC_CXX_DATA_MODEL_LP64,
                           "LP64 data models must match");
IREE_STATIC_ASSERT_ENUM_EQ(LOOM_CXX_DATA_MODEL_LLP64,
                           LOOMC_CXX_DATA_MODEL_LLP64,
                           "LLP64 data models must match");
IREE_STATIC_ASSERT_ENUM_EQ(LOOM_CXX_DATA_MODEL_ILP32,
                           LOOMC_CXX_DATA_MODEL_ILP32,
                           "ILP32 data models must match");
IREE_STATIC_ASSERT_ENUM_EQ(LOOM_CXX_FP_MODEL_PRECISE,
                           LOOMC_CXX_FP_MODEL_PRECISE,
                           "precise FP models must match");
IREE_STATIC_ASSERT_ENUM_EQ(LOOM_CXX_FP_MODEL_FAST, LOOMC_CXX_FP_MODEL_FAST,
                           "fast FP models must match");
IREE_STATIC_ASSERT_ENUM_EQ(LOOM_CXX_IMPORT_FLAG_NO_BUILTIN_INCLUDES,
                           LOOMC_CXX_IMPORT_FLAG_NO_BUILTIN_INCLUDES,
                           "builtin-include flags must match");

static iree_status_t loom_cxx_input_copy_strings(
    const iree_string_view_t* source, iree_host_size_t count,
    iree_arena_allocator_t* arena, loomc_string_view_t** out_target) {
  *out_target = NULL;
  if (count == 0) {
    return iree_ok_status();
  }
  loomc_string_view_t* target = NULL;
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(arena, count, sizeof(*target),
                                                 (void**)&target));
  for (iree_host_size_t i = 0; i < count; ++i) {
    target[i] = loomc_string_view_from_iree(source[i]);
  }
  *out_target = target;
  return iree_ok_status();
}

typedef struct loom_cxx_input_source_path_mapper_t {
  // Command-line source path policy borrowed during this import.
  const loom_tooling_source_path_options_t* options;
  // Invocation-local storage retaining each mapped identifier.
  iree_arena_allocator_t* arena;
} loom_cxx_input_source_path_mapper_t;

static loomc_status_t loom_cxx_input_map_source_path(
    void* user_data, loomc_string_view_t path,
    loomc_string_view_t* out_identifier) {
  loom_cxx_input_source_path_mapper_t* mapper =
      (loom_cxx_input_source_path_mapper_t*)user_data;
  iree_string_view_t identifier = iree_string_view_empty();
  char* identifier_storage = NULL;
  loomc_status_t status = loomc_status_from_iree(loom_tooling_source_path_remap(
      iree_string_view_from_loomc(path), mapper->options,
      iree_arena_allocator(mapper->arena), &identifier, &identifier_storage));
  if (loomc_status_is_ok(status)) {
    *out_identifier = loomc_string_view_from_iree(identifier);
  }
  return status;
}

iree_status_t loom_cxx_input_import_loomc(
    loomc_context_t* context, loomc_workspace_t* workspace,
    const loomc_source_t* source, iree_string_view_t input_options,
    const loom_tooling_source_path_options_t* source_path_options,
    iree_arena_block_pool_t* block_pool, iree_allocator_t host_allocator,
    loomc_module_t** out_module, loomc_result_t** out_result) {
  iree_arena_allocator_t arena;
  iree_arena_initialize(block_pool, &arena);

  loom_cxx_import_options_t parsed_options;
  iree_status_t status = loom_cxx_input_parse_options(
      iree_string_view_from_loomc(loomc_source_identifier(source)),
      input_options, &arena, host_allocator, &parsed_options);

  loomc_cxx_import_options_t options = {
      .type = LOOMC_STRUCTURE_TYPE_CXX_IMPORT_OPTIONS,
      .structure_size = sizeof(options),
      .standard = loomc_string_view_from_iree(parsed_options.standard),
      .triple = loomc_string_view_from_iree(parsed_options.triple),
      .data_model = (loomc_cxx_data_model_t)parsed_options.data_model,
      .fp_model = (loomc_cxx_fp_model_t)parsed_options.fp_model,
      .flags = parsed_options.flags,
      .include_path_count = parsed_options.include_path_count,
      .system_include_path_count = parsed_options.system_include_path_count,
      .define_count = parsed_options.define_count,
      .root_count = parsed_options.root_count,
  };
  loomc_string_view_t* include_paths = NULL;
  loomc_string_view_t* system_include_paths = NULL;
  loomc_string_view_t* roots = NULL;
  loomc_cxx_define_t* defines = NULL;
  loom_cxx_input_source_path_mapper_t source_path_mapper = {
      .options = source_path_options,
      .arena = &arena,
  };
  if (iree_status_is_ok(status)) {
    status = loom_cxx_input_copy_strings(parsed_options.include_paths,
                                         parsed_options.include_path_count,
                                         &arena, &include_paths);
  }
  if (iree_status_is_ok(status)) {
    status =
        loom_cxx_input_copy_strings(parsed_options.system_include_paths,
                                    parsed_options.system_include_path_count,
                                    &arena, &system_include_paths);
  }
  if (iree_status_is_ok(status)) {
    status = loom_cxx_input_copy_strings(
        parsed_options.roots, parsed_options.root_count, &arena, &roots);
  }
  if (iree_status_is_ok(status) && parsed_options.define_count != 0) {
    status = iree_arena_allocate_array(&arena, parsed_options.define_count,
                                       sizeof(*defines), (void**)&defines);
  }
  for (iree_host_size_t i = 0;
       iree_status_is_ok(status) && i < parsed_options.define_count; ++i) {
    defines[i] = (loomc_cxx_define_t){
        .name = loomc_string_view_from_iree(parsed_options.defines[i].name),
        .value = loomc_string_view_from_iree(parsed_options.defines[i].value),
    };
  }
  if (iree_status_is_ok(status)) {
    options.include_paths = include_paths;
    options.system_include_paths = system_include_paths;
    options.defines = defines;
    options.roots = roots;
    if (source_path_options->prefix_maps.count != 0) {
      options.source_path_mapper = (loomc_cxx_source_path_mapper_t){
          .fn = loom_cxx_input_map_source_path,
          .user_data = &source_path_mapper,
      };
    }
    status = iree_status_from_loomc(loomc_module_import_cxx(
        context, workspace, source, &options,
        loomc_allocator_from_iree(host_allocator), out_module, out_result));
  }

  iree_arena_deinitialize(&arena);
  return status;
}
