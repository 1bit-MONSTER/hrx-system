// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/tooling/execution/hal/execution_backend.h"

#include <string.h>

#include "iree/base/alignment.h"
#include "iree/base/byte_sequence.h"
#include "iree/base/internal/arena.h"
#include "loom/pass/pipeline_snapshot.h"
#include "loom/target/entry_selection.h"
#include "loom/tooling/compile/pipeline.h"
#include "loom/tooling/compile/report_capture.h"
#include "loom/tooling/execution/hal/artifact.h"
#include "loom/tooling/execution/hal/candidate.h"
#include "loom/tooling/execution/hal/invocation.h"
#include "loom/tooling/execution/hal/runtime.h"
#include "loom/tooling/io/file.h"

static const loom_run_hal_execution_backend_t*
loom_run_hal_execution_backend_from_base(
    const loom_run_execution_backend_t* backend) {
  IREE_ASSERT_ARGUMENT(backend);
  return iree_containerof(backend, loom_run_hal_execution_backend_t, base);
}

static const loom_device_provider_t*
loom_run_hal_execution_backend_device_provider(
    const loom_run_execution_backend_t* backend) {
  const loom_run_hal_execution_backend_t* hal_execution_backend =
      loom_run_hal_execution_backend_from_base(backend);
  return hal_execution_backend->device_provider;
}

static bool loom_run_hal_execution_backend_accept_entry(
    void* user_data, const loom_target_entry_t* entry) {
  (void)user_data;
  (void)entry;
  return true;
}

static iree_status_t loom_run_hal_execution_backend_select_entry(
    const loom_run_one_shot_request_t* request, iree_arena_allocator_t* arena,
    bool* out_selected, loom_target_entry_t* out_entry) {
  *out_selected = false;
  *out_entry = (loom_target_entry_t){0};

  const loom_target_entry_options_t options = {
      .entry_symbol = request->options->hal_function_name,
      .diagnostic_sink = request->compile_options->diagnostic_sink,
      .source_resolver = request->compile_options->source_resolver,
      .max_errors = request->compile_options->max_errors,
  };
  loom_target_entry_diagnostic_emitter_t diagnostic_emitter = {0};
  loom_target_entry_diagnostic_emitter_initialize(
      request->run_module->module, &options, LOOM_EMITTER_VERIFIER,
      &diagnostic_emitter);
  const loom_target_entry_predicate_t predicate = {
      .fn = loom_run_hal_execution_backend_accept_entry,
  };
  return loom_target_entry_select_entry(
      request->run_module->module, &options, predicate, &diagnostic_emitter,
      IREE_SV("HAL execution"), arena, out_selected, out_entry);
}

static iree_status_t loom_run_hal_write_artifact(
    iree_string_view_t path, const iree_byte_sequence_t* contents,
    iree_string_view_t artifact_name, iree_allocator_t allocator) {
  if (iree_string_view_is_empty(path)) {
    return iree_ok_status();
  }
  if (contents == NULL || iree_byte_sequence_length(contents) == 0) {
    return iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                            "HAL %.*s artifact is empty",
                            (int)artifact_name.size, artifact_name.data);
  }
  return loom_tooling_write_output_byte_sequence(path, contents, allocator);
}

static iree_status_t loom_run_hal_write_candidate_artifacts(
    const loom_run_one_shot_request_t* request,
    const loom_run_hal_candidate_t* candidate) {
  IREE_RETURN_IF_ERROR(loom_run_hal_write_artifact(
      request->options->hal_target_artifact_output_path,
      candidate->artifact.target_artifact_data, IREE_SV("target-native"),
      request->host_allocator));
  return loom_run_hal_write_artifact(
      request->options->hal_executable_output_path,
      candidate->artifact.executable_data, IREE_SV("executable"),
      request->host_allocator);
}

static iree_status_t loom_run_hal_execution_backend_select_device_target(
    const loom_device_provider_t* device_provider,
    const loom_run_hal_runtime_t* runtime,
    const loom_run_one_shot_request_t* request,
    const loom_target_facts_t* authored_target_requirement,
    bool* out_owns_target, loom_device_target_t* out_target) {
  *out_owns_target = false;
  *out_target = (loom_device_target_t){0};

  const iree_string_view_t target_specification =
      iree_string_view_trim(request->target);
  if (iree_string_view_is_empty(target_specification)) {
    IREE_RETURN_IF_ERROR(loom_device_provider_select_compatible_target(
        device_provider, runtime, authored_target_requirement,
        request->host_allocator, out_target));
    *out_owns_target = true;
    return iree_ok_status();
  }

  if (request->target_environment == NULL) {
    return iree_make_status(
        IREE_STATUS_FAILED_PRECONDITION,
        "explicit HAL target selection requires a target environment");
  }
  loom_artifact_target_t artifact_target = {0};
  IREE_RETURN_IF_ERROR(loom_artifact_target_select(
      device_provider->artifact_provider, request->target_environment,
      target_specification, &artifact_target));
  return loom_device_provider_select_profile_target(
      device_provider, runtime, artifact_target.target_profile, out_target);
}

static iree_status_t loom_run_hal_execution_backend_run_pipeline(
    const loom_run_one_shot_request_t* request,
    loom_run_module_t* compile_module,
    const loom_pass_pipeline_snapshot_t* pipeline_snapshot,
    const loom_compile_options_t* compile_options,
    const loom_target_entry_t* entry, const loom_device_target_t* device_target,
    loom_compile_pipeline_result_t* out_result) {
  const loom_target_specialization_request_t specialization_request = {
      .function_name = entry->func_name,
      .target_profile = device_target->artifact_target.target_profile,
  };
  loom_compile_pipeline_options_t pipeline_options = {0};
  loom_compile_pipeline_options_initialize(&pipeline_options);
  pipeline_options.pipeline = request->pipeline;
  if (pipeline_snapshot != NULL) {
    pipeline_options.named_pipeline.module = pipeline_snapshot->module;
    pipeline_options.named_pipeline.pipeline_op =
        pipeline_snapshot->pipeline_op;
  }
  pipeline_options.target_pipeline_options =
      compile_options->target_pipeline_options;
  pipeline_options.target_environment = request->target_environment;
  pipeline_options.target_specializations =
      (loom_target_specialization_request_list_t){
          .values = &specialization_request,
          .count = 1,
      };
  pipeline_options.low_descriptor_registry =
      loom_run_session_low_descriptor_registry(request->session);
  pipeline_options.cleanup_pattern_provider_set =
      loom_run_session_cleanup_pattern_provider_set(request->session);
  pipeline_options.source_resolver =
      loom_run_module_source_resolver(compile_module);
  pipeline_options.report = compile_options->report;
  pipeline_options.diagnostic_sink = compile_options->diagnostic_sink;
  return loom_compile_run_pipeline(
      compile_module->module, &pipeline_options,
      loom_run_session_block_pool(request->session), out_result);
}

iree_status_t loom_run_hal_execution_backend_probe(
    const loom_run_execution_backend_t* backend,
    const loom_run_one_shot_probe_request_t* request) {
  const loom_device_provider_t* device_provider =
      loom_run_hal_execution_backend_device_provider(backend);
  if (device_provider->select_target == NULL) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "device provider '%.*s' is missing required target "
                            "selection hook",
                            (int)device_provider->artifact_provider->name.size,
                            device_provider->artifact_provider->name.data);
  }

  loom_run_hal_runtime_t runtime = {0};
  loom_device_target_t target = {0};

  loom_run_hal_runtime_options_t runtime_options;
  loom_run_hal_runtime_options_initialize(device_provider->driver_name,
                                          &runtime_options);
  iree_status_t status = loom_run_hal_runtime_initialize(
      &runtime_options, request->host_allocator, &runtime);
  if (iree_status_is_ok(status)) {
    status = device_provider->select_target(device_provider, &runtime,
                                            request->host_allocator, &target);
  }
  if (iree_status_is_ok(status)) {
    status = iree_string_builder_append_format(
        &request->result->output,
        "backend: %.*s\ndevice provider: %.*s\nhal driver: %.*s\n",
        (int)backend->name.size, backend->name.data,
        (int)device_provider->artifact_provider->name.size,
        device_provider->artifact_provider->name.data,
        (int)device_provider->driver_name.size,
        device_provider->driver_name.data);
  }
  if (iree_status_is_ok(status) &&
      !iree_string_view_is_empty(target.artifact_target.target_key)) {
    status = iree_string_builder_append_format(
        &request->result->output, "device target key: %.*s\n",
        (int)target.artifact_target.target_key.size,
        target.artifact_target.target_key.data);
  }

  if (device_provider->deinitialize_target != NULL) {
    device_provider->deinitialize_target(device_provider, &target,
                                         request->host_allocator);
  }
  loom_run_hal_runtime_deinitialize(&runtime);
  return status;
}

iree_status_t loom_run_hal_execution_backend_run_one_shot(
    const loom_run_execution_backend_t* backend,
    const loom_run_one_shot_request_t* request) {
  if (request->session == NULL || request->target_environment == NULL) {
    return iree_make_status(
        IREE_STATUS_FAILED_PRECONDITION,
        "HAL compilation requires a session and target environment");
  }
  const loom_device_provider_t* device_provider =
      loom_run_hal_execution_backend_device_provider(backend);
  loom_compile_options_t compile_options = *request->compile_options;
  const loom_sanitizer_options_t sanitizer =
      compile_options.target_pipeline_options.sanitizer;
  compile_options.target_pipeline_options =
      device_provider->artifact_provider->default_pipeline_options;
  compile_options.target_pipeline_options.sanitizer = sanitizer;

  loom_run_hal_runtime_t runtime = {0};
  loom_device_target_t device_target = {0};
  bool owns_device_target = false;
  loom_compile_pipeline_result_t pipeline_result = {0};
  loom_pass_pipeline_snapshot_t pipeline_snapshot = {0};
  loom_run_module_t compile_module = {0};
  loom_run_hal_candidate_t candidate = {0};
  loom_run_hal_invocation_result_t invocation_result = {0};
  loom_run_hal_invocation_result_initialize(request->host_allocator,
                                            &invocation_result);

  iree_arena_block_pool_t block_pool;
  iree_arena_block_pool_initialize(32 * 1024, request->host_allocator,
                                   &block_pool);
  iree_arena_allocator_t arena;
  iree_arena_initialize(&block_pool, &arena);
  bool entry_selected = false;
  loom_target_entry_t entry = {0};
  iree_status_t status = loom_run_hal_execution_backend_select_entry(
      request, &arena, &entry_selected, &entry);
  if (iree_status_is_ok(status) && !entry_selected) {
    request->result->exit_code = 1;
  }

  iree_string_view_t export_name = iree_string_view_empty();
  if (iree_status_is_ok(status) && entry_selected) {
    const loom_string_id_t export_symbol =
        loom_func_like_export_symbol(entry.func);
    export_name =
        export_symbol != LOOM_STRING_ID_INVALID
            ? loom_string_table_get(&request->run_module->module->strings,
                                    export_symbol)
            : entry.func_name;
  }

  loom_run_hal_runtime_options_t runtime_options;
  loom_run_hal_runtime_options_initialize(device_provider->driver_name,
                                          &runtime_options);
  const bool has_named_pipeline =
      loom_compile_pipeline_is_named(request->pipeline);
  if (iree_status_is_ok(status) && entry_selected && has_named_pipeline) {
    status = loom_pass_pipeline_snapshot_initialize(
        request->run_module->module, request->pipeline,
        IREE_SV("__loom_run_pipeline"), &block_pool, request->host_allocator,
        &pipeline_snapshot);
  }
  if (iree_status_is_ok(status) && entry_selected) {
    status = loom_run_module_clone(
        request->session, request->run_module,
        (iree_string_view_list_t){.count = 1, .values = &entry.func_name},
        &compile_module);
  }
  if (iree_status_is_ok(status) && entry_selected) {
    status = loom_run_hal_runtime_features_query(
        compile_module.module,
        &compile_options.target_pipeline_options.sanitizer,
        request->host_allocator, &runtime_options.runtime_features);
  }
  if (iree_status_is_ok(status) && entry_selected) {
    status = loom_run_hal_runtime_initialize(&runtime_options,
                                             request->host_allocator, &runtime);
  }
  if (iree_status_is_ok(status) && entry_selected) {
    status = loom_run_hal_execution_backend_select_device_target(
        device_provider, &runtime, request, entry.target_facts,
        &owns_device_target, &device_target);
  }
  if (iree_status_is_ok(status) && entry_selected) {
    compile_options.source_resolver =
        loom_run_module_source_resolver(&compile_module);
    status = loom_run_hal_execution_backend_run_pipeline(
        request, &compile_module,
        has_named_pipeline ? &pipeline_snapshot : NULL, &compile_options,
        &entry, &device_target, &pipeline_result);
  }
  if (iree_status_is_ok(status) && entry_selected &&
      pipeline_result.pass.error_count != 0) {
    request->result->exit_code = 1;
  }
  if (iree_status_is_ok(status) && entry_selected &&
      pipeline_result.pass.error_count == 0) {
    compile_options.function_versions = &pipeline_result.function_versions.list;
    status = loom_run_hal_candidate_emit_target(
        device_provider, &device_target, &compile_module, &compile_options,
        request->host_allocator, &candidate);
  }
  if (iree_status_is_ok(status) && entry_selected &&
      pipeline_result.pass.error_count == 0 && !candidate.compiled) {
    request->result->exit_code = 1;
  }
  if (iree_status_is_ok(status) && candidate.compiled) {
    status = loom_run_hal_write_candidate_artifacts(request, &candidate);
  }
  if (iree_status_is_ok(status) && candidate.compiled &&
      !request->options->hal_emit_only) {
    const loom_device_artifact_t device_artifact = {
        .executable_target = candidate.device_target.executable_target,
        .artifact = &candidate.artifact,
    };
    loom_run_hal_invocation_request_t invocation_request = {0};
    loom_run_hal_invocation_request_initialize(&invocation_request);
    invocation_request.runtime = &runtime;
    invocation_request.artifact = &device_artifact;
    invocation_request.options.function_name = export_name;
    invocation_request.options.workgroup_count[0] =
        request->options->hal_workgroup_count[0];
    invocation_request.options.workgroup_count[1] =
        request->options->hal_workgroup_count[1];
    invocation_request.options.workgroup_count[2] =
        request->options->hal_workgroup_count[2];
    if (request->options->hal_constant_count >
        IREE_ARRAYSIZE(invocation_request.options.constants)) {
      status = iree_make_status(
          IREE_STATUS_OUT_OF_RANGE,
          "HAL dispatch constant count %" PRIhsz " exceeds maximum %" PRIhsz,
          request->options->hal_constant_count,
          IREE_ARRAYSIZE(invocation_request.options.constants));
    }
    if (iree_status_is_ok(status)) {
      invocation_request.options.constant_count =
          request->options->hal_constant_count;
      memcpy(invocation_request.options.constants,
             request->options->hal_constants,
             request->options->hal_constant_count *
                 sizeof(request->options->hal_constants[0]));
      invocation_request.bindings = (loom_run_hal_binding_specs_t){
          .values = request->options->hal_bindings.values,
          .count = request->options->hal_bindings.count,
      };
      invocation_request.expected_bindings = (loom_run_hal_binding_specs_t){
          .values = request->options->hal_expected_bindings.values,
          .count = request->options->hal_expected_bindings.count,
      };
      invocation_request.max_output_element_count =
          request->options->hal_max_output_element_count;
      status = loom_run_hal_invocation_run(
          &invocation_request, request->host_allocator, &invocation_result);
    }
  }
  if (iree_status_is_ok(status) && candidate.compiled &&
      !request->options->hal_emit_only) {
    request->result->exit_code = invocation_result.exit_code;
    status = iree_string_builder_append_string(
        &request->result->output,
        iree_string_builder_view(&invocation_result.output));
  }
  if (iree_status_is_ok(status) && request->compile_report_capture != NULL) {
    status = loom_compile_report_capture_append_output(
        request->compile_report_capture, &request->result->output);
  }

  loom_run_hal_invocation_result_deinitialize(&invocation_result);
  loom_run_hal_candidate_deinitialize(&candidate);
  if (owns_device_target && device_provider->deinitialize_target != NULL) {
    device_provider->deinitialize_target(device_provider, &device_target,
                                         request->host_allocator);
  }
  loom_compile_pipeline_result_deinitialize(&pipeline_result);
  loom_pass_pipeline_snapshot_deinitialize(&pipeline_snapshot);
  loom_run_module_deinitialize(&compile_module);
  loom_run_hal_runtime_deinitialize(&runtime);
  iree_arena_deinitialize(&arena);
  iree_arena_block_pool_deinitialize(&block_pool);
  return status;
}
