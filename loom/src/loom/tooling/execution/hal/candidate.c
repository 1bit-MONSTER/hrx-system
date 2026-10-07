// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/tooling/execution/hal/candidate.h"

#include "loom/target/entry_selection.h"

static void loom_run_hal_candidate_initialize(
    const loom_device_target_t* target,
    loom_run_hal_candidate_t* out_candidate) {
  *out_candidate = (loom_run_hal_candidate_t){
      .executable_target = target->executable_target,
  };
}

static void loom_run_hal_candidate_initialize_report(
    const loom_device_provider_t* provider,
    const loom_compile_options_t* options) {
  loom_target_compile_report_t* report = options->report;
  if (report == NULL) {
    return;
  }
  loom_target_compile_report_initialize_if_empty(report, report->allocator);
  report->artifact_kind = LOOM_TARGET_COMPILE_ARTIFACT_KIND_HAL_EXECUTABLE;
  report->backend_name = provider->name;
  report->target_family_name = provider->target_profile_type->name;
}

static void loom_run_hal_candidate_record_report_status(
    const loom_device_provider_t* provider, const loom_device_target_t* target,
    const loom_compile_options_t* options,
    const loom_run_hal_candidate_t* candidate, iree_status_code_t status_code) {
  loom_target_compile_report_t* report = options->report;
  if (report == NULL) {
    return;
  }
  report->artifact_kind = LOOM_TARGET_COMPILE_ARTIFACT_KIND_HAL_EXECUTABLE;
  report->backend_name = provider->name;
  report->target_family_name = provider->target_profile_type->name;
  report->target_key = candidate->compiled
                           ? target->executable_target->target_key
                           : iree_string_view_empty();
  if (candidate->compiled) {
    report->artifact_format = loom_target_artifact_format_name(
        candidate->artifact.target_artifact_format);
    loom_target_compile_report_record_artifact_size(
        report, iree_byte_sequence_length(candidate->artifact.contents));
  }
  loom_target_compile_report_record_status(report, status_code);
}

static iree_status_t loom_run_hal_candidate_emit(
    const loom_device_provider_t* provider, loom_run_session_t* session,
    loom_run_module_t* run_module, const loom_compile_options_t* options,
    iree_allocator_t allocator, loom_run_hal_candidate_t* candidate) {
  const loom_target_emitter_t* emitter = provider->target_emitter;
  IREE_ASSERT(emitter != NULL && emitter->emit != NULL);

  const loom_target_entry_options_t target_options = {
      .function_versions = options->function_versions,
      .diagnostic_sink = options->diagnostic_sink,
      .source_resolver = options->source_resolver,
      .max_errors = options->max_errors,
  };
  loom_target_entry_diagnostic_emitter_t diagnostic_emitter = {0};
  loom_target_entry_diagnostic_emitter_initialize(
      run_module->module, &target_options, LOOM_EMITTER_VERIFIER,
      &diagnostic_emitter);

  iree_arena_allocator_t scratch_arena;
  iree_arena_initialize(loom_run_session_block_pool(session), &scratch_arena);
  const loom_target_low_descriptor_registry_t low_descriptor_registry =
      loom_target_environment_low_descriptor_registry(
          session->target_environment);
  loom_target_emit_request_flags_t emit_flags =
      LOOM_TARGET_EMIT_REQUEST_FLAG_RETAIN_TARGET_BUNDLE;
  if (iree_any_bit_set(options->artifact_flags,
                       LOOM_COMPILE_ARTIFACT_FLAG_TARGET_LISTING)) {
    emit_flags |= LOOM_TARGET_EMIT_REQUEST_FLAG_TARGET_LISTING;
  }
  const loom_target_emit_request_t request = {
      .target_environment = session->target_environment,
      .low_descriptor_registry = &low_descriptor_registry.registry,
      .module = run_module->module,
      .function_versions = options->function_versions,
      .identifier = options->artifact_manifest.artifact_name,
      .artifact_manifest =
          {
              .mode = options->artifact_manifest.mode,
              .identifier = options->artifact_manifest.identifier,
          },
      .flags = emit_flags,
      .compile_report = options->report,
      .diagnostic_emitter = loom_target_entry_emitter(&diagnostic_emitter),
      .max_errors = options->max_errors,
      .scratch_arena = &scratch_arena,
      .allocator = allocator,
  };
  iree_status_t status =
      emitter->emit(&request, &candidate->compiled, &candidate->artifact);
  if (iree_status_is_ok(status) && candidate->compiled) {
    IREE_ASSERT(candidate->artifact.target_bundle != NULL);
    IREE_ASSERT(candidate->artifact.target_artifact_format ==
                emitter->target_artifact_format);
    IREE_ASSERT(candidate->artifact.contents != NULL);
    IREE_ASSERT_GT(iree_byte_sequence_length(candidate->artifact.contents), 0);
    IREE_ASSERT(candidate->artifact.sidecar_count == 0 ||
                candidate->artifact.sidecars != NULL);
  }
  iree_arena_deinitialize(&scratch_arena);
  return status;
}

iree_status_t loom_run_hal_candidate_emit_target(
    const loom_device_provider_t* provider, const loom_device_target_t* target,
    loom_run_session_t* session, loom_run_module_t* run_module,
    const loom_compile_options_t* options, iree_allocator_t allocator,
    loom_run_hal_candidate_t* out_candidate) {
  loom_run_hal_candidate_initialize(target, out_candidate);
  loom_run_hal_candidate_initialize_report(provider, options);
  iree_status_t status = loom_run_hal_candidate_emit(
      provider, session, run_module, options, allocator, out_candidate);
  loom_run_hal_candidate_record_report_status(
      provider, target, options, out_candidate, iree_status_code(status));
  if (!iree_status_is_ok(status)) {
    loom_run_hal_candidate_deinitialize(out_candidate);
  }
  return status;
}

void loom_run_hal_candidate_deinitialize(loom_run_hal_candidate_t* candidate) {
  if (candidate == NULL) {
    return;
  }
  loom_target_emit_artifact_release(&candidate->artifact);
  *candidate = (loom_run_hal_candidate_t){0};
}
