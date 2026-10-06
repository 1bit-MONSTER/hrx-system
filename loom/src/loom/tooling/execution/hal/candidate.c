// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/tooling/execution/hal/candidate.h"

static void loom_run_hal_candidate_initialize(
    const loom_device_provider_t* provider, const loom_device_target_t* target,
    const loom_compile_options_t* options, iree_allocator_t allocator,
    loom_run_hal_candidate_t* out_candidate) {
  *out_candidate = (loom_run_hal_candidate_t){
      .host_allocator = allocator,
      .provider = provider,
      .device_target = *target,
  };
  loom_target_compile_report_t* report = options->report;
  if (report == NULL) {
    return;
  }
  const loom_artifact_provider_t* artifact_provider =
      provider->artifact_provider;
  loom_target_compile_report_initialize_if_empty(report, report->allocator);
  report->artifact_kind = artifact_provider->artifact_kind;
  report->backend_name = artifact_provider->name;
  report->target_family_name = artifact_provider->target_profile_type->name;
}

static void loom_run_hal_candidate_record_report_status(
    const loom_compile_options_t* options,
    const loom_run_hal_candidate_t* candidate, iree_status_code_t status_code) {
  loom_target_compile_report_t* report = options->report;
  if (report == NULL) {
    return;
  }
  const loom_artifact_provider_t* artifact_provider =
      candidate->provider->artifact_provider;
  report->artifact_kind = artifact_provider->artifact_kind;
  report->backend_name = artifact_provider->name;
  report->target_family_name = artifact_provider->target_profile_type->name;
  if (candidate->compiled) {
    report->target_key = candidate->device_target.artifact_target.target_key;
    report->artifact_format = loom_target_artifact_format_name(
        candidate->artifact.target_artifact_format);
    if (candidate->artifact.executable_data != NULL) {
      loom_target_compile_report_record_artifact_size(
          report,
          iree_byte_sequence_length(candidate->artifact.executable_data));
    }
  }
  loom_target_compile_report_record_status(report, status_code);
}

static iree_status_t loom_run_hal_candidate_emit(
    loom_run_module_t* run_module, const loom_compile_options_t* options,
    loom_run_hal_candidate_t* candidate) {
  const loom_artifact_provider_t* artifact_provider =
      candidate->provider->artifact_provider;
  if (artifact_provider->emit_artifact == NULL) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "artifact provider '%.*s' is missing required "
                            "emit hook",
                            (int)artifact_provider->name.size,
                            artifact_provider->name.data);
  }

  iree_status_t status = artifact_provider->emit_artifact(
      artifact_provider, run_module->module,
      &candidate->device_target.artifact_target, options,
      candidate->host_allocator, &candidate->compiled, &candidate->artifact);
  if (iree_status_is_ok(status) && candidate->compiled) {
    IREE_ASSERT(candidate->artifact.target_bundle != NULL);
    IREE_ASSERT(candidate->artifact.target_artifact_data != NULL);
    IREE_ASSERT_GT(
        iree_byte_sequence_length(candidate->artifact.target_artifact_data), 0);
    IREE_ASSERT(candidate->artifact.executable_data != NULL);
    IREE_ASSERT_GT(
        iree_byte_sequence_length(candidate->artifact.executable_data), 0);
    IREE_ASSERT(candidate->artifact.sidecar_count == 0 ||
                candidate->artifact.sidecars != NULL);
  }
  return status;
}

iree_status_t loom_run_hal_candidate_emit_target(
    const loom_device_provider_t* provider, const loom_device_target_t* target,
    loom_run_module_t* run_module, const loom_compile_options_t* options,
    iree_allocator_t allocator, loom_run_hal_candidate_t* out_candidate) {
  if (target == NULL) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "HAL candidate target emission requires a "
                            "selected target");
  }
  loom_run_hal_candidate_initialize(provider, target, options, allocator,
                                    out_candidate);
  iree_status_t status =
      loom_run_hal_candidate_emit(run_module, options, out_candidate);
  loom_run_hal_candidate_record_report_status(options, out_candidate,
                                              iree_status_code(status));
  if (!iree_status_is_ok(status)) {
    loom_run_hal_candidate_deinitialize(out_candidate);
  }
  return status;
}

void loom_run_hal_candidate_deinitialize(loom_run_hal_candidate_t* candidate) {
  if (candidate == NULL) {
    return;
  }
  if (candidate->provider != NULL) {
    const loom_artifact_provider_t* artifact_provider =
        candidate->provider->artifact_provider;
    if (artifact_provider != NULL &&
        artifact_provider->deinitialize_artifact != NULL) {
      artifact_provider->deinitialize_artifact(
          artifact_provider, &candidate->artifact, candidate->host_allocator);
    }
  }
  *candidate = (loom_run_hal_candidate_t){0};
}
