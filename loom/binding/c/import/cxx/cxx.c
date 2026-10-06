// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loomc/import/cxx.h"

#include "context.h"
#include "diagnostic.h"
#include "loom/import/cxx/import.h"
#include "loomc/iree.h"
#include "module.h"
#include "result.h"
#include "target.h"

IREE_STATIC_ASSERT_ENUM_EQ(LOOMC_CXX_DATA_MODEL_LP64, LOOM_CXX_DATA_MODEL_LP64,
                           "public LP64 layout matches the native importer");
IREE_STATIC_ASSERT_ENUM_EQ(LOOMC_CXX_DATA_MODEL_LLP64,
                           LOOM_CXX_DATA_MODEL_LLP64,
                           "public LLP64 layout matches the native importer");
IREE_STATIC_ASSERT_ENUM_EQ(LOOMC_CXX_DATA_MODEL_ILP32,
                           LOOM_CXX_DATA_MODEL_ILP32,
                           "public ILP32 layout matches the native importer");
IREE_STATIC_ASSERT_ENUM_EQ(
    LOOMC_CXX_IMPORT_FLAG_APPROXIMATE_FUNCTIONS,
    LOOM_CXX_IMPORT_FLAG_APPROXIMATE_FUNCTIONS,
    "public approximate-functions flag matches the native importer");
IREE_STATIC_ASSERT_ENUM_EQ(
    LOOMC_CXX_IMPORT_FLAG_NO_BUILTIN_INCLUDES,
    LOOM_CXX_IMPORT_FLAG_NO_BUILTIN_INCLUDES,
    "public no-builtin-includes flag matches the native importer");

// Invocation-local adaptation; no frontend state escapes the native import.
typedef struct loomc_cxx_invocation_t {
  // Result receiving copied diagnostics and their source contents.
  loomc_result_t* result;
  // Caller-owned source provider configuration.
  loomc_cxx_source_provider_t provider;
  // Most recent provider reference, valid until the next callback or teardown.
  loomc_source_t* provided_source;
} loomc_cxx_invocation_t;

static loomc_status_t loomc_cxx_validate_string(loomc_string_view_t value) {
  if (value.size && !value.data) {
    return loomc_make_status(LOOMC_STATUS_INVALID_ARGUMENT,
                             "C/C++ option string has length but no data");
  }
  return loomc_ok_status();
}

static loomc_status_t loomc_cxx_copy_strings(loomc_host_size_t count,
                                             const loomc_string_view_t* source,
                                             iree_string_view_t* target) {
  loomc_status_t status = loomc_ok_status();
  for (loomc_host_size_t i = 0; i < count && loomc_status_is_ok(status); ++i) {
    status = loomc_cxx_validate_string(source[i]);
    if (loomc_status_is_ok(status)) {
      target[i] = iree_string_view_from_loomc(source[i]);
    }
  }
  return status;
}

// Converts arrays into one allocation, preserving strict aliasing between the
// independent public and native view types. The caller frees storage even when
// source option validation fails partway through conversion.
static loomc_status_t loomc_cxx_resolve_options(
    const loomc_cxx_import_options_t* options, loomc_allocator_t allocator,
    loom_cxx_import_options_t* out_options, void** out_storage) {
  loom_cxx_import_options_initialize(out_options);
  *out_storage = NULL;
  if (!options) {
    return loomc_ok_status();
  }
  if ((options->type != LOOMC_STRUCTURE_TYPE_NONE &&
       options->type != LOOMC_STRUCTURE_TYPE_CXX_IMPORT_OPTIONS) ||
      (options->structure_size && options->structure_size < sizeof(*options))) {
    return loomc_make_status(LOOMC_STATUS_INVALID_ARGUMENT,
                             "invalid C/C++ import options descriptor");
  }
  if (options->next) {
    return loomc_make_status(LOOMC_STATUS_UNIMPLEMENTED,
                             "C/C++ import option extensions are unsupported");
  }
  LOOMC_RETURN_IF_ERROR(loomc_cxx_validate_string(options->standard));
  LOOMC_RETURN_IF_ERROR(loomc_cxx_validate_string(options->triple));
  if ((!options->include_paths && options->include_path_count) ||
      (!options->system_include_paths && options->system_include_path_count) ||
      (!options->defines && options->define_count) ||
      (!options->roots && options->root_count)) {
    return loomc_make_status(LOOMC_STATUS_INVALID_ARGUMENT,
                             "nonempty C/C++ option arrays require storage");
  }
  out_options->standard = iree_string_view_from_loomc(options->standard);
  out_options->triple = iree_string_view_from_loomc(options->triple);
  out_options->data_model = (loom_cxx_data_model_t)options->data_model;
  out_options->flags = options->flags;
  out_options->include_path_count = options->include_path_count;
  out_options->system_include_path_count = options->system_include_path_count;
  out_options->root_count = options->root_count;
  out_options->define_count = options->define_count;

  iree_host_size_t allocation_size = 0;
  iree_host_size_t includes_offset = 0, system_offset = 0, roots_offset = 0;
  iree_host_size_t defines_offset = 0;
  LOOMC_RETURN_IF_ERROR(loomc_status_from_iree(IREE_STRUCT_LAYOUT(
      0, &allocation_size,
      IREE_STRUCT_FIELD(options->include_path_count, iree_string_view_t,
                        &includes_offset),
      IREE_STRUCT_FIELD(options->system_include_path_count, iree_string_view_t,
                        &system_offset),
      IREE_STRUCT_FIELD(options->root_count, iree_string_view_t, &roots_offset),
      IREE_STRUCT_FIELD(options->define_count, loom_cxx_define_t,
                        &defines_offset))));
  if (!allocation_size) {
    return loomc_ok_status();
  }
  LOOMC_RETURN_IF_ERROR(loomc_allocator_malloc_uninitialized(
      allocator, allocation_size, out_storage));
  uint8_t* storage = (uint8_t*)*out_storage;
  iree_string_view_t* includes =
      (iree_string_view_t*)(storage + includes_offset);
  iree_string_view_t* system = (iree_string_view_t*)(storage + system_offset);
  iree_string_view_t* roots = (iree_string_view_t*)(storage + roots_offset);
  loom_cxx_define_t* defines = (loom_cxx_define_t*)(storage + defines_offset);
  out_options->include_paths = includes;
  out_options->system_include_paths = system;
  out_options->roots = roots;
  out_options->defines = defines;
  LOOMC_RETURN_IF_ERROR(loomc_cxx_copy_strings(
      options->include_path_count, options->include_paths, includes));
  LOOMC_RETURN_IF_ERROR(
      loomc_cxx_copy_strings(options->system_include_path_count,
                             options->system_include_paths, system));
  LOOMC_RETURN_IF_ERROR(
      loomc_cxx_copy_strings(options->root_count, options->roots, roots));
  loomc_status_t status = loomc_ok_status();
  for (loomc_host_size_t i = 0;
       i < options->define_count && loomc_status_is_ok(status); ++i) {
    status = loomc_cxx_validate_string(options->defines[i].name);
    if (loomc_status_is_ok(status)) {
      status = loomc_cxx_validate_string(options->defines[i].value);
    }
    if (loomc_status_is_ok(status)) {
      defines[i].name = iree_string_view_from_loomc(options->defines[i].name);
      defines[i].value = iree_string_view_from_loomc(options->defines[i].value);
    }
  }
  return status;
}

static iree_status_t loomc_cxx_provide_source(void* user_data,
                                              iree_string_view_t path,
                                              bool* out_found,
                                              iree_string_view_t* out_source) {
  loomc_cxx_invocation_t* invocation = (loomc_cxx_invocation_t*)user_data;
  *out_found = false;
  *out_source = iree_string_view_empty();
  loomc_source_release(invocation->provided_source);
  invocation->provided_source = NULL;
  IREE_RETURN_IF_ERROR(iree_status_from_loomc(invocation->provider.fn(
      invocation->provider.user_data, loomc_string_view_from_iree(path),
      &invocation->provided_source)));
  if (invocation->provided_source) {
    if (loomc_source_format(invocation->provided_source) !=
        LOOMC_SOURCE_FORMAT_UNKNOWN) {
      return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                              "C/C++ headers require UNKNOWN source format");
    }
    loomc_byte_span_t contents =
        loomc_source_contents(invocation->provided_source);
    *out_source =
        iree_make_string_view((const char*)contents.data, contents.data_length);
    *out_found = true;
  }
  return iree_ok_status();
}

static iree_status_t loomc_cxx_capture_diagnostic(
    void* user_data, const loom_diagnostic_t* diagnostic) {
  loomc_cxx_invocation_t* invocation = (loomc_cxx_invocation_t*)user_data;
  return iree_status_from_loomc(
      loomc_result_add_loom_diagnostic(invocation->result, NULL, diagnostic));
}

loomc_status_t loomc_module_import_cxx(
    loomc_context_t* context, loomc_workspace_t* workspace,
    const loomc_source_t* source, const loomc_cxx_import_options_t* options,
    loomc_allocator_t allocator, loomc_module_t** out_module,
    loomc_result_t** out_result) {
  LOOMC_RETURN_IF_ERROR(loomc_module_validate_source_arguments(
      context, workspace, source, out_module, out_result));
  if (loomc_source_format(source) != LOOMC_SOURCE_FORMAT_UNKNOWN) {
    return loomc_make_status(LOOMC_STATUS_INVALID_ARGUMENT,
                             "C/C++ import requires UNKNOWN source format");
  }
  loomc_module_t* module = NULL;
  loom_module_t* internal_module = NULL;
  loom_cxx_import_options_t native_options;
  void* option_storage = NULL;
  loomc_cxx_invocation_t invocation = {0};
  loomc_status_t status = loomc_cxx_resolve_options(
      options, allocator, &native_options, &option_storage);
  if (loomc_status_is_ok(status)) {
    status = loomc_result_create(LOOMC_RESULT_STATE_SUCCEEDED, allocator,
                                 &invocation.result);
  }
  if (loomc_status_is_ok(status)) {
    status = loomc_module_create_empty(context, workspace, allocator, &module);
  }
  if (loomc_status_is_ok(status)) {
    native_options.diagnostic_sink =
        (loom_diagnostic_sink_t){loomc_cxx_capture_diagnostic, &invocation};
    loomc_target_pass_environment_initialize_text_asm_environment(
        loomc_context_target_pass_environment(context),
        &native_options.low_asm_environment);
    if (options && options->source_provider.fn) {
      invocation.provider = options->source_provider;
      native_options.source_provider =
          (loom_cxx_source_provider_t){loomc_cxx_provide_source, &invocation};
    }
    const loomc_byte_span_t contents = loomc_source_contents(source);
    status = loomc_status_from_iree(loom_cxx_import(
        iree_make_string_view((const char*)contents.data, contents.data_length),
        iree_string_view_from_loomc(loomc_source_identifier(source)),
        loomc_context_loom_context(context), loomc_module_block_pool(module),
        &native_options, iree_allocator_from_loomc(allocator),
        &internal_module));
  }
  if (loomc_status_is_ok(status)) {
    if (internal_module) {
      loomc_module_set_loom_module(module, internal_module,
                                   LOOMC_MODULE_INPUT_STRUCTURALLY_VERIFIED);
      internal_module = NULL;
    } else {
      status =
          loomc_result_set_state(invocation.result, LOOMC_RESULT_STATE_FAILED);
    }
  }
  if (loomc_status_is_ok(status)) {
    if (loomc_result_succeeded(invocation.result)) {
      *out_module = module;
      module = NULL;
    }
    *out_result = invocation.result;
    invocation.result = NULL;
  }
  loom_module_free(internal_module);
  loomc_module_release(module);
  loomc_result_release(invocation.result);
  loomc_source_release(invocation.provided_source);
  loomc_allocator_free(allocator, option_storage);
  return status;
}
