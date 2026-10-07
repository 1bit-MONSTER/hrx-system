// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/import/cxx/tooling/input.h"

#include "iree/base/internal/path.h"
#include "loom/import/cxx/import.h"
#include "loom/tooling/io/file.h"

static iree_status_t loom_cxx_input_token(iree_string_view_t* remaining,
                                          iree_string_builder_t* token) {
  iree_string_builder_reset(token);
  *remaining = iree_string_view_trim(*remaining);
  char quote = 0;
  while (remaining->size) {
    char c = remaining->data[0];
    *remaining = iree_string_view_substr(*remaining, 1, IREE_HOST_SIZE_MAX);
    if (quote) {
      if (c == quote) {
        quote = 0;
        continue;
      }
      if (c == '\\' && remaining->size &&
          (remaining->data[0] == quote || remaining->data[0] == '\\')) {
        c = remaining->data[0];
        *remaining = iree_string_view_substr(*remaining, 1, IREE_HOST_SIZE_MAX);
      }
    } else if (c == '\'' || c == '"') {
      quote = c;
      continue;
    } else if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
      break;
    }
    IREE_RETURN_IF_ERROR(
        iree_string_builder_append_string(token, iree_make_string_view(&c, 1)));
  }
  if (quote) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "unterminated quote in C/C++ input options");
  }
  return iree_ok_status();
}

static iree_status_t loom_cxx_input_options(
    const loom_input_request_t* request, iree_arena_allocator_t* arena,
    iree_allocator_t host_allocator, loom_cxx_import_options_t* options) {
  iree_string_view_t* tokens = NULL;
  iree_host_size_t count = 0;
  iree_host_size_t capacity = 0;
  iree_string_builder_t token;
  iree_string_builder_initialize(host_allocator, &token);
  iree_string_view_t remaining = iree_string_view_trim(request->options);
  iree_status_t status = iree_ok_status();
  while (iree_status_is_ok(status) && remaining.size) {
    status = loom_cxx_input_token(&remaining, &token);
    if (iree_status_is_ok(status) && count == capacity) {
      status = iree_arena_grow_array(arena, count, count + 1, sizeof(*tokens),
                                     &capacity, (void**)&tokens);
    }
    if (iree_status_is_ok(status)) {
      char* storage = NULL;
      status = iree_arena_allocate(arena, token.size, (void**)&storage);
      if (iree_status_is_ok(status)) {
        memcpy(storage, token.buffer, token.size);
        tokens[count++] = iree_make_string_view(storage, token.size);
      }
    }
    remaining = iree_string_view_trim(remaining);
  }
  iree_string_builder_deinitialize(&token);
  IREE_RETURN_IF_ERROR(status);

  iree_string_view_t* include_paths = NULL;
  iree_string_view_t* system_paths = NULL;
  iree_string_view_t* roots = NULL;
  loom_cxx_define_t* defines = NULL;
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      arena, count, sizeof(*include_paths), (void**)&include_paths));
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      arena, count, sizeof(*system_paths), (void**)&system_paths));
  IREE_RETURN_IF_ERROR(
      iree_arena_allocate_array(arena, count, sizeof(*roots), (void**)&roots));
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(arena, count, sizeof(*defines),
                                                 (void**)&defines));
  options->include_paths = include_paths;
  options->system_include_paths = system_paths;
  options->roots = roots;
  options->defines = defines;
  for (iree_host_size_t i = 0; i < count; ++i) {
    iree_string_view_t name, value;
    if (iree_string_view_split(tokens[i], '=', &name, &value) < 0 ||
        iree_string_view_is_empty(value)) {
      return iree_make_status(
          IREE_STATUS_INVALID_ARGUMENT,
          "expected C/C++ input option key=value, got '%.*s'",
          (int)tokens[i].size, tokens[i].data);
    }
    if (iree_string_view_equal(name, IREE_SV("std"))) {
      options->standard = value;
    } else if (iree_string_view_equal(name, IREE_SV("triple"))) {
      options->triple = value;
    } else if (iree_string_view_equal(name, IREE_SV("data-model"))) {
      if (iree_string_view_equal(value, IREE_SV("lp64"))) {
        options->data_model = LOOM_CXX_DATA_MODEL_LP64;
      } else if (iree_string_view_equal(value, IREE_SV("llp64"))) {
        options->data_model = LOOM_CXX_DATA_MODEL_LLP64;
      } else if (iree_string_view_equal(value, IREE_SV("ilp32"))) {
        options->data_model = LOOM_CXX_DATA_MODEL_ILP32;
      } else {
        return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                                "unknown source data model '%.*s'",
                                (int)value.size, value.data);
      }
    } else if (iree_string_view_equal(name, IREE_SV("root"))) {
      roots[options->root_count++] = value;
    } else if (iree_string_view_equal(name, IREE_SV("I")) ||
               iree_string_view_equal(name, IREE_SV("isystem"))) {
      char* path = NULL;
      IREE_RETURN_IF_ERROR(loom_tooling_file_path_join(
          iree_file_path_dirname(request->path), value,
          iree_arena_allocator(arena), &path));
      if (iree_string_view_equal(name, IREE_SV("I"))) {
        include_paths[options->include_path_count++] =
            iree_make_cstring_view(path);
      } else {
        system_paths[options->system_include_path_count++] =
            iree_make_cstring_view(path);
      }
    } else if (iree_string_view_equal(name, IREE_SV("D"))) {
      loom_cxx_define_t* define = &defines[options->define_count++];
      if (iree_string_view_split(value, '=', &define->name, &define->value) <
          0) {
        define->value = IREE_SV("1");
      }
    } else if (iree_string_view_equal(name, IREE_SV("approximate-functions")) ||
               iree_string_view_equal(name, IREE_SV("builtin-includes"))) {
      bool enabled = iree_string_view_equal(value, IREE_SV("true"));
      if (!enabled && !iree_string_view_equal(value, IREE_SV("false"))) {
        return iree_make_status(
            IREE_STATUS_INVALID_ARGUMENT,
            "C/C++ input option '%.*s' expects true or false", (int)name.size,
            name.data);
      }
      loom_cxx_import_flags_t flag =
          iree_string_view_equal(name, IREE_SV("approximate-functions"))
              ? LOOM_CXX_IMPORT_FLAG_APPROXIMATE_FUNCTIONS
              : LOOM_CXX_IMPORT_FLAG_NO_BUILTIN_INCLUDES;
      if (flag == LOOM_CXX_IMPORT_FLAG_NO_BUILTIN_INCLUDES) {
        enabled = !enabled;
      }
      if (enabled) {
        options->flags |= flag;
      } else {
        options->flags &= ~flag;
      }
    } else {
      return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                              "unknown C/C++ input option '%.*s'",
                              (int)name.size, name.data);
    }
  }
  return iree_ok_status();
}

static iree_status_t loom_cxx_input_load(const loom_input_request_t* request,
                                         loom_input_source_capture_t capture,
                                         loom_context_t* context,
                                         iree_arena_block_pool_t* block_pool,
                                         iree_allocator_t host_allocator,
                                         loom_module_t** out_module) {
  iree_arena_allocator_t arena;
  iree_arena_initialize(block_pool, &arena);
  loom_cxx_import_options_t options;
  loom_cxx_import_options_initialize(&options);
  options.diagnostic_sink = request->parse_options.diagnostic_sink;
  options.low_asm_environment = request->parse_options.low_asm_environment;
  if (capture.fn) {
    options.source_observer = (loom_cxx_source_observer_t){
        .fn = capture.fn, .user_data = capture.user_data};
  }
  iree_status_t status =
      loom_cxx_input_options(request, &arena, host_allocator, &options);
  if (iree_status_is_ok(status)) {
    status = loom_cxx_import(request->source, request->path, context,
                             block_pool, &options, host_allocator, out_module);
  }
  iree_arena_deinitialize(&arena);
  return status;
}

static const iree_string_view_t loom_cxx_input_suffixes[] = {
    IREE_SVL(".cxx-test"), IREE_SVL(".cxx"), IREE_SVL(".cpp"),
    IREE_SVL(".cc"),       IREE_SVL(".c"),
};

const loom_input_provider_t loom_cxx_input_provider = {
    .name = IREE_SVL("cxx"),
    .suffixes = {IREE_ARRAYSIZE(loom_cxx_input_suffixes),
                 loom_cxx_input_suffixes},
    .load = loom_cxx_input_load,
};
