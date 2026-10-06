// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/verify/verify.h"

#include "loom/codegen/low/text_asm.h"
#include "loom/codegen/low/verify.h"
#include "loom/error/source.h"
#include "loom/format/text/parser.h"
#include "loom/ir/module.h"
#include "loom/target/provider.h"
#include "loom/tools/loom-check/diagnostics.h"
#include "loom/tools/loom-check/execute.h"
#include "loom/tools/loom-check/input.h"

//===----------------------------------------------------------------------===//
// Verify execution
//===----------------------------------------------------------------------===//

iree_status_t loom_check_execute_verify(
    const loom_test_case_t* test_case, iree_host_size_t case_index,
    loom_check_file_report_t* report, iree_string_view_t filename,
    const loom_input_request_t* input_request,
    const loom_check_environment_t* environment, loom_context_t* context,
    iree_arena_block_pool_t* block_pool, iree_allocator_t allocator,
    loom_check_result_t* result) {
  // Arena for collector storage. Entries and message copies are released
  // together at the end of this case.
  iree_arena_allocator_t collector_arena;
  iree_arena_initialize(block_pool, &collector_arena);

  loom_check_diagnostic_collector_t collector = {
      .arena = &collector_arena,
      .host_allocator = allocator,
      .filename = filename,
      .result = result,
  };

  loom_input_module_t input = {0};
  loom_module_t* module = NULL;
  loom_target_low_descriptor_registry_t low_registry;
  iree_status_t status =
      loom_check_environment_initialize_low_descriptor_registry(environment,
                                                                &low_registry);
  if (iree_status_is_ok(status)) {
    loom_low_descriptor_text_print_context_initialize(
        &low_registry.registry, &collector.type_print_context);
  }
  if (iree_status_is_ok(status)) {
    loom_text_parse_options_t parse_options = {
        .diagnostic_sink = {.fn = loom_check_diagnostic_collector_sink,
                            .user_data = &collector},
        .max_errors = 100,
    };
    loom_low_descriptor_text_asm_environment_storage_t low_asm_storage = {0};
    loom_low_descriptor_text_asm_environment_initialize_with_diagnostics(
        &low_registry.registry,
        loom_target_environment_low_asm_diagnostic_provider_list(
            environment->target_environment),
        &low_asm_storage, &parse_options.low_asm_environment);
    status =
        loom_check_load_input(test_case, input_request, environment, context,
                              block_pool, &parse_options, allocator, &input);
    module = input.module;
    collector.module = module;
  }

  // Later verification resolves locations against the retained source set.
  if (iree_status_is_ok(status) && module) {
    loom_source_resolver_t source_resolver =
        loom_input_module_source_resolver(&input);
    loom_verify_options_t verify_options = {
        .sink = {.fn = loom_check_diagnostic_collector_sink,
                 .user_data = &collector},
        .max_errors = 100,
        .source_resolver = source_resolver,
    };

    loom_verify_result_t verify_result = {0};
    if (iree_status_is_ok(status)) {
      status = loom_verify_module(module, &verify_options, &verify_result);
    }
    if (iree_status_is_ok(status) && verify_result.error_count == 0) {
      loom_check_diagnostic_emitter_capture_t low_diagnostic_capture = {
          .diagnostic_collector = &collector,
          .module = module,
          .source_resolver = source_resolver,
          .emitter = LOOM_EMITTER_VERIFIER,
      };
      loom_low_verify_options_t low_verify_options = {
          .descriptor_registry = &low_registry.registry,
          .emitter =
              {
                  .fn = loom_check_diagnostic_emitter_capture_emit,
                  .user_data = &low_diagnostic_capture,
              },
          .provider_list = loom_target_environment_low_verify_provider_list(
              environment->target_environment),
          .max_errors = 100,
      };
      loom_low_verify_result_t low_verify_result = {0};
      loom_low_verify_scratch_t low_verify_scratch =
          loom_low_verify_scratch_for_module(module);
      status = loom_low_verify_module(module, &low_verify_options,
                                      &low_verify_scratch, &low_verify_result);
    }
  }

  if (iree_status_is_ok(status)) {
    status = loom_check_diagnostic_collector_finish(
        &collector, test_case, case_index, report, allocator, result);
  }

  loom_input_module_deinitialize(&input);
  iree_arena_deinitialize(&collector_arena);
  return status;
}
