// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/tools/iree-test-loom/xfail.h"

#include <stdio.h>
#include <string.h>

static iree_status_t iree_test_loom_parse_diagnostic_ref(
    iree_string_view_t value, loom_error_ref_t* out_ref) {
  *out_ref = LOOM_ERROR_REF_NONE;
  iree_string_view_t domain_text = iree_string_view_empty();
  iree_string_view_t code_text = iree_string_view_empty();
  if (iree_string_view_split(value, '/', &domain_text, &code_text) < 0 ||
      iree_string_view_is_empty(domain_text) ||
      iree_string_view_is_empty(code_text)) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "expected diagnostic identity DOMAIN/NNN, got "
                            "'%.*s'",
                            (int)value.size, value.data);
  }

  loom_error_domain_t domain = LOOM_ERROR_DOMAIN_COUNT_;
  if (!loom_error_domain_from_name(domain_text, &domain)) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "unknown diagnostic domain '%.*s'",
                            (int)domain_text.size, domain_text.data);
  }
  uint32_t code = 0;
  if (!iree_string_view_atoi_uint32_base(code_text, 10, &code) || code == 0 ||
      code > LOOM_ERROR_REF_CODE_MASK) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "invalid diagnostic code '%.*s'",
                            (int)code_text.size, code_text.data);
  }
  char canonical_code[6] = {0};
  const int canonical_code_length =
      snprintf(canonical_code, sizeof(canonical_code), "%03u", code);
  if (canonical_code_length < 0 ||
      !iree_string_view_equal(
          code_text,
          iree_make_string_view(canonical_code, canonical_code_length))) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "diagnostic code must use canonical zero-padded "
                            "spelling, got '%.*s'",
                            (int)code_text.size, code_text.data);
  }
  // Target diagnostics live in optional catalog shards that the test runner
  // does not own. Execution still fails loud for an unknown identity because
  // no emitted diagnostic can match the parsed reference.
  const loom_error_ref_t ref = LOOM_ERROR_REF(domain, code);
  *out_ref = ref;
  return iree_ok_status();
}

iree_status_t iree_test_loom_xfail_list_initialize(
    iree_string_view_list_t values, iree_allocator_t allocator,
    iree_test_loom_xfail_list_t* out_list) {
  *out_list = (iree_test_loom_xfail_list_t){0};
  if (values.count == 0) {
    return iree_ok_status();
  }
  IREE_RETURN_IF_ERROR(iree_allocator_malloc_array(allocator, values.count,
                                                   sizeof(*out_list->values),
                                                   (void**)&out_list->values));
  memset(out_list->values, 0, values.count * sizeof(*out_list->values));
  out_list->count = values.count;
  for (iree_host_size_t i = 0; i < values.count; ++i) {
    iree_string_view_t record = iree_string_view_empty();
    iree_string_view_t diagnostic = iree_string_view_empty();
    if (iree_string_view_split(values.values[i], '=', &record, &diagnostic) <
            0 ||
        record.size < 2 || record.data[0] != '@' ||
        iree_string_view_is_empty(diagnostic)) {
      return iree_make_status(
          IREE_STATUS_INVALID_ARGUMENT,
          "--xfail value '%.*s' must use '@record=DOMAIN/NNN'",
          (int)values.values[i].size, values.values[i].data);
    }
    for (iree_host_size_t j = 0; j < i; ++j) {
      if (iree_string_view_equal(out_list->values[j].record, record)) {
        return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                                "--xfail repeats record '%.*s'",
                                (int)record.size, record.data);
      }
    }
    iree_test_loom_xfail_t* xfail = &out_list->values[i];
    xfail->record = record;
    xfail->diagnostic = diagnostic;
    IREE_RETURN_IF_ERROR(iree_test_loom_parse_diagnostic_ref(
        diagnostic, &xfail->diagnostic_ref));
  }
  return iree_ok_status();
}

void iree_test_loom_xfail_list_deinitialize(iree_test_loom_xfail_list_t* list,
                                            iree_allocator_t allocator) {
  iree_allocator_free(allocator, list->values);
  *list = (iree_test_loom_xfail_list_t){0};
}

static iree_string_view_t iree_test_loom_xfail_record_name(
    const iree_test_loom_xfail_t* xfail) {
  return iree_string_view_substr(xfail->record, 1, IREE_HOST_SIZE_MAX);
}

iree_test_loom_xfail_t* iree_test_loom_xfail_list_find(
    iree_test_loom_xfail_list_t* list, iree_string_view_t record_name) {
  for (iree_host_size_t i = 0; i < list->count; ++i) {
    if (iree_string_view_equal(
            iree_test_loom_xfail_record_name(&list->values[i]), record_name)) {
      return &list->values[i];
    }
  }
  return NULL;
}

iree_status_t iree_test_loom_validate_xfails(
    iree_test_loom_xfail_list_t* xfails,
    const loom_testbench_module_plan_t* module_plan,
    iree_string_view_t selected_record_name) {
  for (iree_host_size_t xfail_index = 0; xfail_index < xfails->count;
       ++xfail_index) {
    const iree_string_view_t record_name =
        iree_test_loom_xfail_record_name(&xfails->values[xfail_index]);
    iree_host_size_t match_count = 0;
    for (iree_host_size_t i = 0; i < module_plan->case_count; ++i) {
      match_count +=
          iree_string_view_equal(module_plan->cases[i].name, record_name);
    }
    for (iree_host_size_t i = 0; i < module_plan->scenario_count; ++i) {
      match_count +=
          iree_string_view_equal(module_plan->scenarios[i].name, record_name);
    }
    if (match_count != 1) {
      return iree_make_status(
          IREE_STATUS_NOT_FOUND,
          "--xfail record '%.*s' matched %zu check records; expected exactly "
          "one",
          (int)xfails->values[xfail_index].record.size,
          xfails->values[xfail_index].record.data, match_count);
    }
    if (!iree_string_view_is_empty(selected_record_name) &&
        !iree_string_view_equal(record_name, selected_record_name)) {
      return iree_make_status(
          IREE_STATUS_INVALID_ARGUMENT,
          "--xfail record '%.*s' is not selected by --case=@%.*s",
          (int)xfails->values[xfail_index].record.size,
          xfails->values[xfail_index].record.data,
          (int)selected_record_name.size, selected_record_name.data);
    }
  }
  return iree_ok_status();
}

void iree_test_loom_diagnostic_capture_begin(
    iree_test_loom_diagnostic_capture_t* capture,
    iree_test_loom_xfail_t* active_xfail) {
  capture->active_xfail = active_xfail;
  capture->first_error_ref = LOOM_ERROR_REF_NONE;
  capture->matched_expected_diagnostic = false;
}

static iree_status_t iree_test_loom_capture_diagnostic(
    void* user_data, const loom_diagnostic_t* diagnostic) {
  iree_test_loom_diagnostic_capture_t* capture =
      (iree_test_loom_diagnostic_capture_t*)user_data;
  if (diagnostic->severity == LOOM_DIAGNOSTIC_ERROR) {
    const loom_error_ref_t ref = loom_error_def_ref(diagnostic->error);
    if (!loom_error_ref_is_set(capture->first_error_ref)) {
      capture->first_error_ref = ref;
    }
    if (capture->active_xfail != NULL &&
        ref == capture->active_xfail->diagnostic_ref) {
      capture->matched_expected_diagnostic = true;
    }
  }
  return loom_diagnostic_stderr_sink(NULL, diagnostic);
}

loom_diagnostic_sink_t iree_test_loom_diagnostic_capture_sink(
    iree_test_loom_diagnostic_capture_t* capture) {
  return (loom_diagnostic_sink_t){
      .fn = iree_test_loom_capture_diagnostic,
      .user_data = capture,
  };
}

void iree_test_loom_capture_expectation_report(
    iree_test_loom_diagnostic_capture_t* capture,
    const loom_testbench_expectation_report_t* report) {
  if (capture->active_xfail == NULL || report == NULL) {
    return;
  }
  for (iree_host_size_t i = 0; i < report->failure_count; ++i) {
    const loom_error_ref_t ref = report->failures[i].diagnostic_ref;
    if (!loom_error_ref_is_set(capture->first_error_ref)) {
      capture->first_error_ref = ref;
    }
    if (ref == capture->active_xfail->diagnostic_ref) {
      capture->matched_expected_diagnostic = true;
    }
  }
}

void iree_test_loom_finish_xfail(
    iree_test_loom_xfail_t* xfail,
    const iree_test_loom_diagnostic_capture_t* capture,
    iree_test_loom_xfail_outcome_t outcome,
    iree_host_size_t accepted_failed_sample_count,
    iree_host_size_t accepted_failed_trial_count) {
  xfail->outcome = outcome;
  xfail->observed_diagnostic_ref = capture->first_error_ref;
  xfail->accepted_failed_sample_count = accepted_failed_sample_count;
  xfail->accepted_failed_trial_count = accepted_failed_trial_count;
}

bool iree_test_loom_xfail_try_accept_compile_failure(
    iree_test_loom_xfail_t* xfail,
    const iree_test_loom_diagnostic_capture_t* capture,
    iree_status_t* inout_status) {
  if (xfail == NULL || iree_status_is_ok(*inout_status) ||
      iree_status_code(*inout_status) != IREE_STATUS_FAILED_PRECONDITION ||
      !capture->matched_expected_diagnostic) {
    return false;
  }
  iree_status_free(*inout_status);
  *inout_status = iree_ok_status();
  iree_test_loom_finish_xfail(
      xfail, capture, IREE_TEST_LOOM_XFAIL_OUTCOME_EXPECTED_FAILURE, 0, 0);
  return true;
}

iree_test_loom_xfail_counts_t iree_test_loom_count_xfails(
    const iree_test_loom_xfail_list_t* xfails) {
  iree_test_loom_xfail_counts_t counts = {0};
  for (iree_host_size_t i = 0; i < xfails->count; ++i) {
    const iree_test_loom_xfail_t* xfail = &xfails->values[i];
    switch (xfail->outcome) {
      case IREE_TEST_LOOM_XFAIL_OUTCOME_EXPECTED_FAILURE:
        ++counts.xfail_count;
        counts.accepted_failed_sample_count +=
            xfail->accepted_failed_sample_count;
        counts.accepted_failed_trial_count +=
            xfail->accepted_failed_trial_count;
        break;
      case IREE_TEST_LOOM_XFAIL_OUTCOME_UNEXPECTED_PASS:
        ++counts.xpass_count;
        break;
      case IREE_TEST_LOOM_XFAIL_OUTCOME_DIAGNOSTIC_MISMATCH:
      case IREE_TEST_LOOM_XFAIL_OUTCOME_PENDING:
        ++counts.mismatch_count;
        break;
    }
  }
  return counts;
}

static iree_string_view_t iree_test_loom_xfail_outcome_name(
    iree_test_loom_xfail_outcome_t outcome) {
  switch (outcome) {
    case IREE_TEST_LOOM_XFAIL_OUTCOME_EXPECTED_FAILURE:
      return IREE_SV("xfail");
    case IREE_TEST_LOOM_XFAIL_OUTCOME_UNEXPECTED_PASS:
      return IREE_SV("xpass");
    case IREE_TEST_LOOM_XFAIL_OUTCOME_DIAGNOSTIC_MISMATCH:
      return IREE_SV("diagnostic_mismatch");
    case IREE_TEST_LOOM_XFAIL_OUTCOME_PENDING:
    default:
      return IREE_SV("not_executed");
  }
}

iree_status_t iree_test_loom_write_xfails_json(
    const iree_test_loom_xfail_list_t* xfails,
    loom_json_object_writer_t* report) {
  IREE_RETURN_IF_ERROR(loom_json_object_begin_field(report, IREE_SV("xfails")));
  loom_json_array_writer_t array;
  IREE_RETURN_IF_ERROR(loom_json_array_begin(report->stream, &array));
  for (iree_host_size_t i = 0; i < xfails->count; ++i) {
    const iree_test_loom_xfail_t* xfail = &xfails->values[i];
    IREE_RETURN_IF_ERROR(loom_json_array_begin_element(&array));
    loom_json_object_writer_t object;
    IREE_RETURN_IF_ERROR(loom_json_object_begin(array.stream, &object));
    IREE_RETURN_IF_ERROR(loom_json_object_write_string_field(
        &object, IREE_SV("record"), xfail->record));
    IREE_RETURN_IF_ERROR(loom_json_object_write_string_field(
        &object, IREE_SV("diagnostic"), xfail->diagnostic));
    IREE_RETURN_IF_ERROR(loom_json_object_write_string_field(
        &object, IREE_SV("outcome"),
        iree_test_loom_xfail_outcome_name(xfail->outcome)));
    if (xfail->outcome == IREE_TEST_LOOM_XFAIL_OUTCOME_DIAGNOSTIC_MISMATCH &&
        loom_error_ref_is_set(xfail->observed_diagnostic_ref)) {
      char observed_diagnostic[32] = {0};
      const int length = snprintf(
          observed_diagnostic, sizeof(observed_diagnostic), "%s/%03u",
          loom_error_domain_name(
              loom_error_ref_domain(xfail->observed_diagnostic_ref)),
          (unsigned)loom_error_ref_code(xfail->observed_diagnostic_ref));
      if (length < 0 ||
          (iree_host_size_t)length >= sizeof(observed_diagnostic)) {
        return iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                                "diagnostic identity exceeded report storage");
      }
      IREE_RETURN_IF_ERROR(loom_json_object_write_string_field(
          &object, IREE_SV("observed_diagnostic"),
          iree_make_string_view(observed_diagnostic, length)));
    }
    IREE_RETURN_IF_ERROR(loom_json_object_end(&object));
  }
  return loom_json_array_end(&array);
}
