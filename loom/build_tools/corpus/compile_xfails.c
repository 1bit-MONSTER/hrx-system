// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "iree/base/api.h"
#include "iree/base/tooling/flags.h"
#include "iree/io/file_contents.h"
#include "loom/build_tools/corpus/diagnostic_report.h"
#include "loom/target/tool/process.h"

IREE_FLAG(string, compiler, "", "Exact filesystem path to loom-compile.");
IREE_FLAG_LIST_NAMED(string, expected_root, "expected-root",
                     "Root expected to fail. Repeat once per xfail.");
IREE_FLAG_LIST_NAMED(
    string, expected_diagnostic, "expected-diagnostic",
    "Required compiler diagnostic identity as DOMAIN/NNN. Repeat in the same "
    "order as --expected-root.");
IREE_FLAG_NAMED(
    bool, require_all_roots, "require-all-roots", false,
    "Require the expected roots to exhaust the compiler's default root set.");
IREE_FLAG_NAMED(string, stamp_output, "stamp-output", "",
                "Output stamp written only after all expected failures.");

static bool loom_corpus_argument_is_flag(iree_string_view_t argument,
                                         iree_string_view_t flag) {
  return iree_string_view_equal(argument, flag) ||
         (argument.size > flag.size && argument.data[flag.size] == '=' &&
          iree_string_view_starts_with(argument, flag));
}

static iree_status_t loom_corpus_validate_compiler_arguments(int argument_count,
                                                             char** arguments) {
  if (argument_count == 0) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "loom-compile arguments are required");
  }
  const iree_string_view_t owned_flags[] = {
      IREE_SV("--output"),
      IREE_SV("--compile-report"),
      IREE_SV("--compile-report-output"),
      IREE_SV("--exclude-root"),
      IREE_SV("--root"),
  };
  for (int i = 0; i < argument_count; ++i) {
    const iree_string_view_t argument = iree_make_cstring_view(arguments[i]);
    for (iree_host_size_t j = 0; j < IREE_ARRAYSIZE(owned_flags); ++j) {
      if (loom_corpus_argument_is_flag(argument, owned_flags[j])) {
        return iree_make_status(
            IREE_STATUS_INVALID_ARGUMENT,
            "corpus diagnostic probe owns compiler flag '%.*s'",
            (int)owned_flags[j].size, owned_flags[j].data);
      }
    }
  }
  return iree_ok_status();
}

static iree_status_t loom_corpus_make_compiler_argument(
    iree_string_view_t prefix, iree_string_view_t value,
    iree_allocator_t allocator, char** out_argument) {
  *out_argument = NULL;
  iree_host_size_t argument_length = 0;
  iree_host_size_t allocation_size = 0;
  if (!iree_host_size_checked_add(prefix.size, value.size, &argument_length) ||
      !iree_host_size_checked_add(argument_length, 1, &allocation_size)) {
    return iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                            "compiler argument is too long");
  }
  char* argument = NULL;
  IREE_RETURN_IF_ERROR(
      iree_allocator_malloc(allocator, allocation_size, (void**)&argument));
  memcpy(argument, prefix.data, prefix.size);
  memcpy(argument + prefix.size, value.data, value.size);
  argument[argument_length] = '\0';
  *out_argument = argument;
  return iree_ok_status();
}

static iree_status_t loom_corpus_validate_expectations(
    iree_flag_string_list_t expected_roots,
    iree_flag_string_list_t expected_diagnostics) {
  if (expected_roots.count == 0) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "at least one --expected-root is required");
  }
  if (expected_roots.count != expected_diagnostics.count) {
    return iree_make_status(
        IREE_STATUS_INVALID_ARGUMENT,
        "--expected-root and --expected-diagnostic counts must match");
  }
  for (iree_host_size_t i = 0; i < expected_roots.count; ++i) {
    const iree_string_view_t root = expected_roots.values[i];
    if (iree_string_view_is_empty(root) || root.data[0] != '@') {
      return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                              "expected root '%.*s' must begin with '@'",
                              (int)root.size, root.data);
    }
    IREE_RETURN_IF_ERROR(loom_corpus_diagnostic_id_list_validate(
        expected_diagnostics.values[i]));
  }
  return iree_ok_status();
}

static void loom_corpus_print_compiler_stderr(
    const loom_tool_output_t* compiler_stderr) {
  if (compiler_stderr->length == 0) {
    return;
  }
  fputs("loom-compile stderr:\n", stderr);
  fwrite(compiler_stderr->data, 1, compiler_stderr->length, stderr);
  if (compiler_stderr->data[compiler_stderr->length - 1] != '\n') {
    fputc('\n', stderr);
  }
}

static bool loom_corpus_compiler_exhausted_default_roots(
    const loom_tool_output_t* compiler_stderr) {
  const iree_string_view_t stderr_text =
      iree_make_string_view(compiler_stderr->data, compiler_stderr->length);
  const iree_string_view_t messages[] = {
      IREE_SV("excluded roots empty the default command root set"),
      IREE_SV("excluded roots empty the default kernel root set"),
      IREE_SV("excluded roots empty the default module root set"),
  };
  for (iree_host_size_t i = 0; i < IREE_ARRAYSIZE(messages); ++i) {
    if (iree_string_view_find(stderr_text, messages[i], 0) !=
        IREE_STRING_VIEW_NPOS) {
      return true;
    }
  }
  return false;
}

static iree_status_t loom_corpus_compile_xfails(int argc, char** argv) {
  const iree_string_view_t compiler_path =
      iree_make_cstring_view(FLAG_compiler);
  const iree_flag_string_list_t expected_roots = FLAG_expected_root_list();
  const iree_flag_string_list_t expected_diagnostics =
      FLAG_expected_diagnostic_list();
  const iree_string_view_t stamp_output_path =
      iree_make_cstring_view(FLAG_stamp_output);
  if (iree_string_view_is_empty(compiler_path)) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "--compiler is required");
  }
  if (iree_string_view_is_empty(stamp_output_path)) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "--stamp-output is required");
  }
  IREE_RETURN_IF_ERROR(
      loom_corpus_validate_compiler_arguments(argc - 1, argv + 1));
  IREE_RETURN_IF_ERROR(
      loom_corpus_validate_expectations(expected_roots, expected_diagnostics));

  iree_allocator_t allocator = iree_allocator_system();
  loom_tool_temp_file_t artifact_file = {0};
  iree_status_t status = loom_tool_temp_file_initialize(
      IREE_SV("loom-corpus-xfail"), &artifact_file);
  const bool artifact_file_initialized = iree_status_is_ok(status);
  char* output_argument = NULL;
  if (iree_status_is_ok(status)) {
    status = loom_corpus_make_compiler_argument(
        IREE_SV("--output="), loom_tool_temp_file_path(&artifact_file),
        allocator, &output_argument);
  }

  const iree_host_size_t compiler_argument_count = (iree_host_size_t)(argc - 1);
  iree_host_size_t common_argument_count = 0;
  iree_host_size_t argument_capacity = 0;
  iree_host_size_t argument_storage_size = 0;
  if (iree_status_is_ok(status) &&
      (!iree_host_size_checked_add(compiler_argument_count, 3,
                                   &common_argument_count) ||
       !iree_host_size_checked_add(common_argument_count, expected_roots.count,
                                   &argument_capacity) ||
       !iree_host_size_checked_mul(argument_capacity,
                                   sizeof(iree_string_view_t),
                                   &argument_storage_size))) {
    status = iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                              "too many compiler arguments");
  }
  iree_string_view_t* compiler_arguments = NULL;
  if (iree_status_is_ok(status)) {
    status = iree_allocator_malloc(allocator, argument_storage_size,
                                   (void**)&compiler_arguments);
  }
  if (iree_status_is_ok(status)) {
    for (iree_host_size_t i = 0; i < compiler_argument_count; ++i) {
      compiler_arguments[i] = iree_make_cstring_view(argv[i + 1]);
    }
    compiler_arguments[compiler_argument_count] =
        IREE_SV("--compile-report=details");
    compiler_arguments[compiler_argument_count + 1] =
        IREE_SV("--compile-report-output=-");
    compiler_arguments[compiler_argument_count + 2] =
        iree_make_cstring_view(output_argument);
  }

  iree_string_builder_t stamp_builder;
  iree_string_builder_initialize(allocator, &stamp_builder);
  for (iree_host_size_t i = 0;
       i < expected_roots.count && iree_status_is_ok(status); ++i) {
    const iree_string_view_t root = expected_roots.values[i];
    const iree_string_view_t diagnostic_text = expected_diagnostics.values[i];

    char* root_argument = NULL;
    if (iree_status_is_ok(status)) {
      status = loom_corpus_make_compiler_argument(IREE_SV("--root="), root,
                                                  allocator, &root_argument);
    }
    if (iree_status_is_ok(status)) {
      compiler_arguments[common_argument_count] =
          iree_make_cstring_view(root_argument);
    }

    loom_tool_process_result_t result = {0};
    if (iree_status_is_ok(status)) {
      status = loom_tool_process_run(
          compiler_path, /*search_path=*/false, compiler_arguments,
          common_argument_count + 1, allocator, &result);
    }
    if (iree_status_is_ok(status)) {
      const iree_string_view_t compile_report_json = iree_make_string_view(
          result.stdout_bytes.data, result.stdout_bytes.length);
      status = loom_corpus_compile_report_expect_diagnostics(
          result.exit_code, compile_report_json, diagnostic_text);
      if (!iree_status_is_ok(status)) {
        loom_corpus_print_compiler_stderr(&result.stderr_bytes);
        status = iree_status_annotate_f(status, "corpus xfail root %.*s",
                                        (int)root.size, root.data);
      }
    }
    if (iree_status_is_ok(status)) {
      status = iree_string_builder_append_format(
          &stamp_builder, "%.*s %.*s\n", (int)root.size, root.data,
          (int)diagnostic_text.size, diagnostic_text.data);
    }
    loom_tool_process_result_deinitialize(&result, allocator);
    iree_allocator_free(allocator, root_argument);
  }

  char** exclusion_arguments = NULL;
  iree_host_size_t exclusion_storage_size = 0;
  if (iree_status_is_ok(status) && FLAG_require_all_roots &&
      (!iree_host_size_checked_mul(expected_roots.count,
                                   sizeof(*exclusion_arguments),
                                   &exclusion_storage_size))) {
    status =
        iree_make_status(IREE_STATUS_OUT_OF_RANGE, "too many expected roots");
  }
  if (iree_status_is_ok(status) && FLAG_require_all_roots) {
    status = iree_allocator_malloc(allocator, exclusion_storage_size,
                                   (void**)&exclusion_arguments);
    if (iree_status_is_ok(status)) {
      memset(exclusion_arguments, 0, exclusion_storage_size);
    }
  }
  for (iree_host_size_t i = 0;
       i < expected_roots.count && iree_status_is_ok(status) &&
       FLAG_require_all_roots;
       ++i) {
    status = loom_corpus_make_compiler_argument(
        IREE_SV("--exclude-root="), expected_roots.values[i], allocator,
        &exclusion_arguments[i]);
    if (iree_status_is_ok(status)) {
      compiler_arguments[common_argument_count + i] =
          iree_make_cstring_view(exclusion_arguments[i]);
    }
  }
  if (iree_status_is_ok(status) && FLAG_require_all_roots) {
    loom_tool_process_result_t result = {0};
    status = loom_tool_process_run(
        compiler_path, /*search_path=*/false, compiler_arguments,
        common_argument_count + expected_roots.count, allocator, &result);
    if (iree_status_is_ok(status) && result.exit_code == 0) {
      status = iree_make_status(
          IREE_STATUS_FAILED_PRECONDITION,
          "declared xfail roots do not exhaust the default root set: compiler "
          "accepted at least one remaining root");
    } else if (iree_status_is_ok(status) &&
               !loom_corpus_compiler_exhausted_default_roots(
                   &result.stderr_bytes)) {
      loom_corpus_print_compiler_stderr(&result.stderr_bytes);
      status = iree_make_status(
          IREE_STATUS_FAILED_PRECONDITION,
          "declared xfail roots do not exhaust the default root set");
    }
    loom_tool_process_result_deinitialize(&result, allocator);
  }
  if (iree_status_is_ok(status)) {
    const iree_string_view_t stamp = iree_string_builder_view(&stamp_builder);
    status = iree_io_file_contents_write(
        stamp_output_path, iree_make_const_byte_span(stamp.data, stamp.size),
        allocator);
  }

  iree_string_builder_deinitialize(&stamp_builder);
  for (iree_host_size_t i = 0; i < expected_roots.count; ++i) {
    if (exclusion_arguments != NULL) {
      iree_allocator_free(allocator, exclusion_arguments[i]);
    }
  }
  iree_allocator_free(allocator, exclusion_arguments);
  iree_allocator_free(allocator, compiler_arguments);
  iree_allocator_free(allocator, output_argument);
  if (artifact_file_initialized) {
    status = iree_status_join(status,
                              loom_tool_temp_file_deinitialize(&artifact_file));
  }
  return status;
}

int main(int argc, char** argv) {
  iree_flags_set_usage(
      "loom-corpus-compile-xfails",
      "Independently runs Loom corpus roots and succeeds only when each emits "
      "its declared compiler diagnostic. Remaining arguments are passed to "
      "loom-compile.");
  iree_flags_parse_checked(IREE_FLAGS_PARSE_MODE_UNDEFINED_OK, &argc, &argv);
  iree_status_t status = loom_corpus_compile_xfails(argc, argv);
  if (iree_status_is_ok(status)) {
    return EXIT_SUCCESS;
  }
  iree_status_fprint(stderr, status);
  iree_status_free(status);
  return EXIT_FAILURE;
}
