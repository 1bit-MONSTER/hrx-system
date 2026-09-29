// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/tooling/target/x86/check/loom_check.h"

#include "loom/target/arch/x86/ops/ops.h"
#include "loom/tooling/target/x86/prepare.h"
#include "loom/tools/loom-check/diagnostics.h"
#include "loom/tools/loom-check/source_low.h"

static bool loom_x86_callable_check_matches(
    const loom_check_emit_provider_t* provider, iree_string_view_t name) {
  (void)provider;
  return iree_string_view_equal(name, IREE_SV("x86-code"));
}

static bool loom_x86_callable_check_accept_entry(
    void* user_data, const loom_target_entry_t* entry) {
  (void)user_data;
  return entry->target_facts->fact_type == &loom_x86_target_fact_type &&
         loom_target_entry_bundle(entry)->export_plan->abi_kind ==
             LOOM_TARGET_ABI_OBJECT_FUNCTION;
}

static iree_status_t loom_x86_callable_check_execute(
    const loom_check_emit_provider_t* provider,
    const loom_check_emit_provider_request_t* request) {
  (void)provider;
  if (!iree_string_view_is_empty(request->target_options)) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "x86-code does not accept options");
  }
  loom_check_prepare_source_low_options_t prepare_options;
  loom_check_prepare_source_low_options_initialize(&prepare_options);
  prepare_options.default_pipeline = LOOM_COMPILE_DEFAULT_PIPELINE_PREPARED_LOW;
  prepare_options.control_flow_lowering = LOOM_TARGET_CONTROL_FLOW_LOWERING_CFG;
  loom_compile_pipeline_result_t pipeline = {0};
  iree_status_t status = loom_check_prepare_source_low_module(
      request->module, &prepare_options, request->low_registry,
      request->environment, request->source_resolver,
      request->diagnostic_collector, request->block_pool, &pipeline);
  loom_check_diagnostic_emitter_capture_t capture = {
      .diagnostic_collector = request->diagnostic_collector,
      .module = request->module,
      .source_resolver = request->source_resolver,
      .emitter = LOOM_EMITTER_PASS,
  };
  const iree_diagnostic_emitter_t emitter = {
      .fn = loom_check_diagnostic_emitter_capture_emit,
      .user_data = &capture,
  };
  const loom_target_entry_options_t options = {
      .function_versions = &pipeline.function_versions.list,
  };
  const loom_target_entry_predicate_t predicate = {
      .fn = loom_x86_callable_check_accept_entry,
  };
  loom_target_entry_list_t entries = {0};
  bool selected = false;
  if (iree_status_is_ok(status) && request->diagnostic_collector->count == 0) {
    status = loom_target_entry_select_all_entries(
        request->module, &options, predicate, emitter, IREE_SV("x86 callable"),
        request->case_arena, &selected, &entries);
  }
  bool prepared = selected;
  for (uint16_t i = 0;
       i < entries.count && prepared && iree_status_is_ok(status); ++i) {
    loom_x86_callable_t callable = {0};
    status = loom_x86_prepare_callable(
        request->module, &entries.values[i], &request->low_registry->registry,
        emitter, request->case_arena, request->case_arena, &prepared,
        &callable);
    iree_string_builder_t* output = &request->result->actual_output;
    if (iree_status_is_ok(status) && prepared) {
      status = iree_string_builder_append_format(output, "%.*s:\n",
                                                 (int)callable.symbol_name.size,
                                                 callable.symbol_name.data);
    }
    for (iree_host_size_t j = 0;
         j < callable.text.contents.data_length && iree_status_is_ok(status);
         ++j) {
      status = iree_string_builder_append_format(
          output, "%s%02x%s", j % 16 == 0 ? "  " : " ",
          callable.text.contents.data[j],
          j % 16 == 15 || j + 1 == callable.text.contents.data_length ? "\n"
                                                                      : "");
    }
  }
  loom_compile_pipeline_result_deinitialize(&pipeline);
  return status;
}

static iree_status_t loom_x86_callable_check_names(
    const loom_check_emit_provider_t* provider, iree_string_builder_t* output) {
  (void)provider;
  return iree_string_builder_append_cstring(output, "x86-code");
}

const loom_check_emit_provider_t loom_x86_callable_check_emit_provider = {
    .name = IREE_SVL("x86-callable"),
    .match = loom_x86_callable_check_matches,
    .execute = loom_x86_callable_check_execute,
    .append_names = loom_x86_callable_check_names,
};
