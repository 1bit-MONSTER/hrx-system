// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/tools/iree-test-loom/library_linker.h"

#include "loom/error/source.h"
#include "loom/link/linker.h"
#include "loom/tooling/io/file.h"
#include "loom/tooling/io/source.h"

typedef struct iree_test_loom_link_sources_t {
  // Snapshots for the module currently being added.
  const loom_source_table_resolver_t* input;
  // Owned snapshots in linked-module source-ID order.
  loom_tooling_source_storage_t output;
} iree_test_loom_link_sources_t;

static iree_status_t iree_test_loom_capture_sources(
    void* user_data, const loom_module_t* source_module,
    const loom_module_t* target_module,
    const loom_source_id_t* target_sources) {
  iree_test_loom_link_sources_t* sources = user_data;
  return loom_tooling_source_storage_project(&sources->output, target_module,
                                             sources->input, target_sources);
}

static iree_status_t iree_test_loom_add_library(
    loom_run_session_t* session, iree_string_view_t library_path,
    const loom_input_options_t* input_options, loom_linker_t* linker,
    iree_test_loom_link_sources_t* sources) {
  if (loom_tooling_file_path_is_stdio(library_path)) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "--library requires a filesystem path");
  }

  iree_io_file_contents_t* contents = NULL;
  loom_run_module_t library_module = {0};
  iree_status_t status = loom_tooling_read_input_file(
      library_path, session->host_allocator, &contents);
  if (iree_status_is_ok(status)) {
    loom_run_module_parse_options_t parse_options = {0};
    loom_run_module_parse_options_initialize(&parse_options);
    if (input_options) {
      parse_options.input = *input_options;
    }
    parse_options.filename = library_path;
    parse_options.source = loom_tooling_file_contents_string_view(contents);
    status = loom_run_module_parse(session, &parse_options, &library_module);
  }
  if (iree_status_is_ok(status)) {
    sources->input = &library_module.sources.table;
    status = loom_linker_add_module(linker, library_module.module,
                                    /*options=*/NULL);
  }
  loom_run_module_deinitialize(&library_module);
  iree_io_file_contents_free(contents);
  return status;
}

iree_status_t iree_test_loom_link_libraries(
    loom_run_session_t* session, loom_run_module_t* run_module,
    iree_string_view_list_t library_paths,
    const loom_input_options_t* input_options) {
  if (library_paths.count == 0) {
    return iree_ok_status();
  }
  if (library_paths.values == NULL) {
    return iree_make_status(
        IREE_STATUS_INVALID_ARGUMENT,
        "library path count is non-zero but values is NULL");
  }

  loom_linker_t* linker = NULL;
  loom_module_t* linked_module = NULL;
  iree_test_loom_link_sources_t sources = {
      .input = &run_module->sources.table,
  };
  loom_tooling_source_storage_initialize(loom_run_session_block_pool(session),
                                         &sources.output);
  const loom_linker_options_t linker_options = {
      .module_name = IREE_SV("linked"),
      .source_callback = {.fn = iree_test_loom_capture_sources,
                          .user_data = &sources},
  };
  iree_status_t status = loom_linker_allocate(
      loom_run_session_context(session), &linker_options,
      loom_run_session_block_pool(session), session->host_allocator, &linker);
  if (iree_status_is_ok(status)) {
    status = loom_linker_add_module(linker, run_module->module,
                                    /*options=*/NULL);
  }
  for (iree_host_size_t i = 0;
       i < library_paths.count && iree_status_is_ok(status); ++i) {
    status = iree_test_loom_add_library(session, library_paths.values[i],
                                        input_options, linker, &sources);
  }
  if (iree_status_is_ok(status)) {
    status = loom_linker_finish(linker, &linked_module);
  }
  if (iree_status_is_ok(status)) {
    loom_module_free(run_module->module);
    run_module->module = linked_module;
    loom_tooling_source_storage_deinitialize(&run_module->sources);
    run_module->sources = sources.output;
    sources.output = (loom_tooling_source_storage_t){0};
    linked_module = NULL;
  }
  loom_module_free(linked_module);
  loom_linker_free(linker);
  loom_tooling_source_storage_deinitialize(&sources.output);
  return status;
}
