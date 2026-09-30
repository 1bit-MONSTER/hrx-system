// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Expected-failure qualification for iree-test-loom records.

#ifndef LOOM_TOOLS_IREE_TEST_LOOM_XFAIL_H_
#define LOOM_TOOLS_IREE_TEST_LOOM_XFAIL_H_

#include "iree/base/api.h"
#include "loom/error/diagnostic.h"
#include "loom/error/error_defs.h"
#include "loom/tooling/testbench/expectation.h"
#include "loom/tooling/testbench/testbench.h"
#include "loom/util/json.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum iree_test_loom_xfail_outcome_e {
  IREE_TEST_LOOM_XFAIL_OUTCOME_PENDING = 0,
  IREE_TEST_LOOM_XFAIL_OUTCOME_EXPECTED_FAILURE,
  IREE_TEST_LOOM_XFAIL_OUTCOME_ALLOWED_PASS,
  IREE_TEST_LOOM_XFAIL_OUTCOME_UNEXPECTED_PASS,
  IREE_TEST_LOOM_XFAIL_OUTCOME_DIAGNOSTIC_MISMATCH,
} iree_test_loom_xfail_outcome_t;

typedef enum iree_test_loom_xfail_policy_e {
  // The record must fail with one of its expected diagnostics.
  IREE_TEST_LOOM_XFAIL_POLICY_STRICT = 0,
  // The record may pass, but any failure must carry an expected diagnostic.
  IREE_TEST_LOOM_XFAIL_POLICY_ALLOW_PASS,
} iree_test_loom_xfail_policy_t;

typedef struct iree_test_loom_xfail_t {
  // Authored record symbol, including its leading '@'.
  iree_string_view_t record;
  // Comma-separated canonical diagnostics supplied on the command line.
  iree_string_view_t diagnostic;
  // Allocator-owned accepted stable diagnostic identities.
  loom_error_ref_t* diagnostic_refs;
  // Number of entries in |diagnostic_refs|.
  iree_host_size_t diagnostic_count;
  // Qualification policy selected by the command-line flag.
  iree_test_loom_xfail_policy_t policy;
  // First failure identity observed while executing this record.
  loom_error_ref_t observed_diagnostic_ref;
  // Number of raw failed samples accepted by this expected failure.
  iree_host_size_t accepted_failed_sample_count;
  // Number of raw failed trials accepted by this expected failure.
  iree_host_size_t accepted_failed_trial_count;
  // Final qualification outcome for this expected failure.
  iree_test_loom_xfail_outcome_t outcome;
} iree_test_loom_xfail_t;

typedef struct iree_test_loom_xfail_list_t {
  // Allocator-owned expected failures in command-line order.
  iree_test_loom_xfail_t* values;
  // Number of entries in |values|.
  iree_host_size_t count;
} iree_test_loom_xfail_list_t;

typedef struct iree_test_loom_diagnostic_capture_t {
  // Expected failure for the record currently executing, or NULL.
  iree_test_loom_xfail_t* active_xfail;
  // First failure identity observed for the active record.
  loom_error_ref_t first_error_ref;
  // True when the active record emitted its expected diagnostic.
  bool matched_expected_diagnostic;
} iree_test_loom_diagnostic_capture_t;

typedef struct iree_test_loom_xfail_counts_t {
  // Records that failed with their expected diagnostic.
  iree_host_size_t xfail_count;
  // Records that unexpectedly passed.
  iree_host_size_t xpass_count;
  // Records that failed without their expected diagnostic.
  iree_host_size_t mismatch_count;
  // Raw failed samples accepted by expected failures.
  iree_host_size_t accepted_failed_sample_count;
  // Raw failed trials accepted by expected failures.
  iree_host_size_t accepted_failed_trial_count;
} iree_test_loom_xfail_counts_t;

// Parses and owns strict and pass-allowed expected failures.
iree_status_t iree_test_loom_xfail_list_initialize(
    iree_string_view_list_t strict_values,
    iree_string_view_list_t allow_failure_values, iree_allocator_t allocator,
    iree_test_loom_xfail_list_t* out_list);

// Releases storage owned by |list|.
void iree_test_loom_xfail_list_deinitialize(iree_test_loom_xfail_list_t* list,
                                            iree_allocator_t allocator);

// Finds the expected failure for |record_name|, which omits the leading '@'.
iree_test_loom_xfail_t* iree_test_loom_xfail_list_find(
    iree_test_loom_xfail_list_t* list, iree_string_view_t record_name);

// Verifies that each expected failure names one selected case or scenario.
iree_status_t iree_test_loom_validate_xfails(
    iree_test_loom_xfail_list_t* xfails,
    const loom_testbench_module_plan_t* module_plan,
    iree_string_view_t selected_record_name);

// Begins diagnostic capture for one record execution.
void iree_test_loom_diagnostic_capture_begin(
    iree_test_loom_diagnostic_capture_t* capture,
    iree_test_loom_xfail_t* active_xfail);

// Returns a diagnostic sink that records stable identities and writes stderr.
loom_diagnostic_sink_t iree_test_loom_diagnostic_capture_sink(
    iree_test_loom_diagnostic_capture_t* capture);

// Captures stable identities from an expectation report.
void iree_test_loom_capture_expectation_report(
    iree_test_loom_diagnostic_capture_t* capture,
    const loom_testbench_expectation_report_t* report);

// Records the final outcome and accepted raw failure counts for |xfail|.
void iree_test_loom_finish_xfail(
    iree_test_loom_xfail_t* xfail,
    const iree_test_loom_diagnostic_capture_t* capture,
    iree_test_loom_xfail_outcome_t outcome,
    iree_host_size_t accepted_failed_sample_count,
    iree_host_size_t accepted_failed_trial_count);

// Accepts and consumes a compiler failure carrying the expected diagnostic.
bool iree_test_loom_xfail_try_accept_compile_failure(
    iree_test_loom_xfail_t* xfail,
    const iree_test_loom_diagnostic_capture_t* capture,
    iree_status_t* inout_status);

// Counts final outcomes and accepted raw failures.
iree_test_loom_xfail_counts_t iree_test_loom_count_xfails(
    const iree_test_loom_xfail_list_t* xfails);

// Writes the expected-failure outcome array into |report|.
iree_status_t iree_test_loom_write_xfails_json(
    const iree_test_loom_xfail_list_t* xfails,
    loom_json_object_writer_t* report);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TOOLS_IREE_TEST_LOOM_XFAIL_H_
