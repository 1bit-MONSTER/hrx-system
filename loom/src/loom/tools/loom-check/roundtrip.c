// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/codegen/low/text_asm.h"
#include "loom/format/text/parser.h"
#include "loom/format/text/printer.h"
#include "loom/ir/module.h"
#include "loom/target/provider.h"
#include "loom/tools/loom-check/comparison.h"
#include "loom/tools/loom-check/execute.h"
#include "loom/tools/loom-check/input.h"

iree_status_t loom_check_validate_printed_ir(
    iree_string_view_t source, loom_context_t* context,
    iree_arena_block_pool_t* block_pool,
    const loom_text_print_options_t* print_options, loom_check_result_t* result,
    bool* out_valid) {
  *out_valid = false;
  loom_check_diagnostic_capture_t diagnostic_capture = {
      .detail = &result->detail,
      .result = result,
  };
  const loom_text_parse_options_t parse_options = {
      .diagnostic_sink = {.fn = loom_check_diagnostic_capture_sink,
                          .user_data = &diagnostic_capture},
      .max_errors = 20,
      .low_asm_environment = print_options->low_asm_environment,
  };
  loom_module_t* module = NULL;
  IREE_RETURN_IF_ERROR(loom_text_parse(source, IREE_SV("<printed IR>"), context,
                                       block_pool, &parse_options, &module));
  if (!module) {
    return iree_string_builder_append_cstring(&result->detail,
                                              "printed IR failed to parse\n");
  }

  iree_string_builder_t reprinted;
  iree_string_builder_initialize(result->detail.allocator, &reprinted);
  iree_status_t status = loom_text_print_module_to_builder_with_options(
      module, &reprinted, print_options);
  loom_module_free(module);
  if (iree_status_is_ok(status)) {
    *out_valid =
        iree_string_view_equal(source, iree_string_builder_view(&reprinted));
    if (!*out_valid) {
      status = iree_string_builder_append_cstring(
          &result->detail, "printed IR changed after reparsing\n");
      if (iree_status_is_ok(status)) {
        status = loom_check_result_record_diff(
            source, iree_string_builder_view(&reprinted),
            result->detail.allocator, result);
      }
    }
  }
  iree_string_builder_deinitialize(&reprinted);
  return status;
}

static loom_text_print_flags_t loom_check_roundtrip_print_flags(
    const loom_test_case_t* test_case) {
  loom_text_print_flags_t flags =
      LOOM_TEXT_PRINT_DEFAULT | LOOM_TEXT_PRINT_PREFER_LOW_ASM;
  if (iree_all_bits_set(test_case->output_flags, LOOM_TEST_OUTPUT_LOCATIONS)) {
    flags |= LOOM_TEXT_PRINT_LOCATIONS;
  }
  return flags;
}

iree_status_t loom_check_execute_roundtrip(
    const loom_test_case_t* test_case, iree_string_view_t filename,
    const loom_input_request_t* input_request,
    const loom_check_environment_t* environment, loom_context_t* context,
    iree_arena_block_pool_t* block_pool, iree_allocator_t allocator,
    loom_check_result_t* result) {
  loom_input_module_t input = {0};
  loom_check_diagnostic_capture_t diagnostic_capture = {
      .detail = &result->detail,
      .result = result,
  };
  loom_text_parse_options_t parse_options = {
      .diagnostic_sink = {.fn = loom_check_diagnostic_capture_sink,
                          .user_data = &diagnostic_capture},
      .max_errors = 20,
  };
  const loom_target_low_descriptor_registry_t low_registry =
      loom_target_environment_low_descriptor_registry(
          environment->target_environment);
  loom_low_descriptor_text_asm_environment_storage_t low_asm_storage = {0};
  loom_low_descriptor_text_asm_environment_initialize_with_diagnostics(
      &low_registry.registry,
      loom_target_environment_low_asm_diagnostic_provider_list(
          environment->target_environment),
      &low_asm_storage, &parse_options.low_asm_environment);
  iree_status_t parse_status =
      loom_check_load_input(test_case, input_request, environment, context,
                            block_pool, &parse_options, allocator, &input);
  if (!iree_status_is_ok(parse_status) || !input.module) {
    result->raw_outcome = LOOM_CHECK_FAIL;
    loom_input_module_deinitialize(&input);
    return parse_status;
  }

  // Print the parsed module to canonical text (directly into the result's
  // actual_output so --update can use it) and free the module.
  loom_text_low_asm_environment_t low_asm_environment = {0};
  loom_low_descriptor_text_asm_environment_initialize(&low_registry.registry,
                                                      &low_asm_environment);
  const loom_text_print_options_t print_options = {
      .flags = loom_check_roundtrip_print_flags(test_case),
      .low_asm_environment = low_asm_environment,
  };
  iree_status_t print_status = loom_text_print_module_to_builder_with_options(
      input.module, &result->actual_output, &print_options);
  loom_input_module_deinitialize(&input);
  IREE_RETURN_IF_ERROR(print_status);
  bool valid_output = false;
  IREE_RETURN_IF_ERROR(loom_check_validate_printed_ir(
      iree_string_builder_view(&result->actual_output), context, block_pool,
      &print_options, result, &valid_output));
  if (!valid_output) {
    result->raw_outcome = LOOM_CHECK_FAIL;
    return iree_ok_status();
  }
  result->has_actual_output = true;

  return loom_check_compare_output(test_case, allocator, result);
}
