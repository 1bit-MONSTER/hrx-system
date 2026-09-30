// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/build_tools/corpus/diagnostic_report.h"

#include <stdio.h>

#include "iree/base/internal/json.h"

iree_status_t loom_corpus_diagnostic_id_parse(
    iree_string_view_t value, loom_corpus_diagnostic_id_t* out_diagnostic_id) {
  *out_diagnostic_id = (loom_corpus_diagnostic_id_t){0};
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
      code > UINT16_MAX) {
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

  out_diagnostic_id->domain = domain;
  out_diagnostic_id->code = (uint16_t)code;
  return iree_ok_status();
}

static bool loom_corpus_diagnostic_id_equal(loom_corpus_diagnostic_id_t lhs,
                                            loom_corpus_diagnostic_id_t rhs) {
  return lhs.domain == rhs.domain && lhs.code == rhs.code;
}

static iree_host_size_t loom_corpus_diagnostic_id_list_count(
    iree_string_view_t value) {
  iree_host_size_t count = 1;
  for (iree_host_size_t i = 0; i < value.size; ++i) {
    count += value.data[i] == ',';
  }
  return count;
}

iree_status_t loom_corpus_diagnostic_id_list_validate(
    iree_string_view_t value) {
  if (iree_string_view_is_empty(value)) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "expected at least one diagnostic identity");
  }
  const iree_host_size_t count = loom_corpus_diagnostic_id_list_count(value);
  iree_string_view_t remaining = value;
  for (iree_host_size_t i = 0; i < count; ++i) {
    iree_string_view_t diagnostic_text = iree_string_view_empty();
    iree_string_view_split(remaining, ',', &diagnostic_text, &remaining);
    if (iree_string_view_is_empty(diagnostic_text)) {
      return iree_make_status(
          IREE_STATUS_INVALID_ARGUMENT,
          "diagnostic list '%.*s' contains an empty identity", (int)value.size,
          value.data);
    }
    loom_corpus_diagnostic_id_t diagnostic_id = {0};
    IREE_RETURN_IF_ERROR(
        loom_corpus_diagnostic_id_parse(diagnostic_text, &diagnostic_id));

    iree_string_view_t prior = value;
    for (iree_host_size_t j = 0; j < i; ++j) {
      iree_string_view_t prior_text = iree_string_view_empty();
      iree_string_view_split(prior, ',', &prior_text, &prior);
      loom_corpus_diagnostic_id_t prior_id = {0};
      IREE_RETURN_IF_ERROR(
          loom_corpus_diagnostic_id_parse(prior_text, &prior_id));
      if (loom_corpus_diagnostic_id_equal(diagnostic_id, prior_id)) {
        return iree_make_status(
            IREE_STATUS_INVALID_ARGUMENT,
            "diagnostic list '%.*s' repeats identity '%.*s'", (int)value.size,
            value.data, (int)diagnostic_text.size, diagnostic_text.data);
      }
    }
  }
  return iree_ok_status();
}

static iree_status_t loom_corpus_diagnostic_id_list_contains(
    iree_string_view_t value, iree_string_view_t domain, uint16_t code,
    bool* out_contains) {
  *out_contains = false;
  iree_string_view_t remaining = value;
  const iree_host_size_t count = loom_corpus_diagnostic_id_list_count(value);
  for (iree_host_size_t i = 0; i < count; ++i) {
    iree_string_view_t diagnostic_text = iree_string_view_empty();
    iree_string_view_split(remaining, ',', &diagnostic_text, &remaining);
    loom_corpus_diagnostic_id_t diagnostic_id = {0};
    IREE_RETURN_IF_ERROR(
        loom_corpus_diagnostic_id_parse(diagnostic_text, &diagnostic_id));
    const iree_string_view_t expected_domain =
        iree_make_cstring_view(loom_error_domain_name(diagnostic_id.domain));
    if (iree_string_view_equal(domain, expected_domain) &&
        code == diagnostic_id.code) {
      *out_contains = true;
      return iree_ok_status();
    }
  }
  return iree_ok_status();
}

typedef struct loom_corpus_diagnostic_match_t {
  iree_string_view_t expected_diagnostics;
  iree_host_size_t match_count;
  iree_string_view_t first_unexpected_domain;
  uint16_t first_unexpected_code;
} loom_corpus_diagnostic_match_t;

static iree_status_t loom_corpus_compile_report_visit_diagnostic(
    void* user_data, iree_host_size_t index, iree_string_view_t value) {
  (void)index;
  loom_corpus_diagnostic_match_t* match =
      (loom_corpus_diagnostic_match_t*)user_data;
  if (iree_string_view_is_empty(value) || value.data[0] != '{') {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "compile report diagnostic must be an object");
  }

  iree_string_view_t severity = iree_string_view_empty();
  IREE_RETURN_IF_ERROR(
      iree_json_lookup_object_value(value, IREE_SV("severity"), &severity));
  if (!iree_string_view_equal(severity, IREE_SV("error"))) {
    return iree_ok_status();
  }

  iree_string_view_t domain = iree_string_view_empty();
  iree_string_view_t code_text = iree_string_view_empty();
  IREE_RETURN_IF_ERROR(
      iree_json_lookup_object_value(value, IREE_SV("domain"), &domain));
  IREE_RETURN_IF_ERROR(
      iree_json_lookup_object_value(value, IREE_SV("code"), &code_text));
  uint64_t code = 0;
  IREE_RETURN_IF_ERROR(iree_json_parse_uint64(code_text, &code));
  if (code > UINT16_MAX) {
    return iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                            "compile report diagnostic code is out of range");
  }

  bool is_expected = false;
  IREE_RETURN_IF_ERROR(loom_corpus_diagnostic_id_list_contains(
      match->expected_diagnostics, domain, (uint16_t)code, &is_expected));
  if (is_expected) {
    ++match->match_count;
  } else if (iree_string_view_is_empty(match->first_unexpected_domain)) {
    match->first_unexpected_domain = domain;
    match->first_unexpected_code = (uint16_t)code;
  }
  return iree_ok_status();
}

static iree_status_t loom_corpus_compile_report_parse_object(
    iree_string_view_t json, iree_string_view_t* out_object) {
  iree_string_view_t remaining = json;
  IREE_RETURN_IF_ERROR(iree_json_consume_value(&remaining, out_object));
  IREE_RETURN_IF_ERROR(iree_json_consume_insignificant(&remaining));
  if (!iree_string_view_is_empty(remaining)) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "compile report has trailing content");
  }
  if (iree_string_view_is_empty(*out_object) || out_object->data[0] != '{') {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "compile report must be a JSON object");
  }
  return iree_ok_status();
}

iree_status_t loom_corpus_compile_report_expect_diagnostics(
    int compiler_exit_code, iree_string_view_t compile_report_json,
    iree_string_view_t expected_diagnostics) {
  IREE_RETURN_IF_ERROR(
      loom_corpus_diagnostic_id_list_validate(expected_diagnostics));
  if (compiler_exit_code == 0) {
    return iree_make_status(
        IREE_STATUS_FAILED_PRECONDITION,
        "XPASS: compiler accepted a root expected to fail with %.*s",
        (int)expected_diagnostics.size, expected_diagnostics.data);
  }
  if (iree_string_view_is_empty(compile_report_json)) {
    return iree_make_status(
        IREE_STATUS_FAILED_PRECONDITION,
        "compiler failed without a structured compile report; expected "
        "%.*s",
        (int)expected_diagnostics.size, expected_diagnostics.data);
  }

  iree_string_view_t report = iree_string_view_empty();
  iree_status_t status =
      loom_corpus_compile_report_parse_object(compile_report_json, &report);
  if (!iree_status_is_ok(status)) {
    return iree_status_annotate_f(
        status,
        "compiler failed without a valid structured compile report; expected "
        "%.*s",
        (int)expected_diagnostics.size, expected_diagnostics.data);
  }

  iree_string_view_t kind = iree_string_view_empty();
  iree_string_view_t mode = iree_string_view_empty();
  IREE_RETURN_IF_ERROR(
      iree_json_lookup_object_value(report, IREE_SV("kind"), &kind));
  IREE_RETURN_IF_ERROR(
      iree_json_lookup_object_value(report, IREE_SV("mode"), &mode));
  if (!iree_string_view_equal(kind, IREE_SV("loom.compile_report")) ||
      !iree_string_view_equal(mode, IREE_SV("details"))) {
    return iree_make_status(
        IREE_STATUS_INVALID_ARGUMENT,
        "compiler failed without a details-mode Loom compile report; expected "
        "%.*s",
        (int)expected_diagnostics.size, expected_diagnostics.data);
  }

  iree_string_view_t diagnostics = iree_string_view_empty();
  IREE_RETURN_IF_ERROR(iree_json_try_lookup_object_value(
      report, IREE_SV("diagnostics"), &diagnostics));
  if (iree_string_view_is_empty(diagnostics)) {
    return iree_make_status(
        IREE_STATUS_FAILED_PRECONDITION,
        "missing diagnostic: compiler exited %d but the compile report "
        "contained no error diagnostics; expected %.*s",
        compiler_exit_code, (int)expected_diagnostics.size,
        expected_diagnostics.data);
  }
  if (diagnostics.data[0] != '[') {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "compile report diagnostics must be an array");
  }

  loom_corpus_diagnostic_match_t match = {
      /*.expected_diagnostics=*/expected_diagnostics,
      /*.match_count=*/0,
      /*.first_unexpected_domain=*/iree_string_view_empty(),
      /*.first_unexpected_code=*/0,
  };
  IREE_RETURN_IF_ERROR(iree_json_enumerate_array(
      diagnostics, loom_corpus_compile_report_visit_diagnostic, &match));
  if (match.match_count == 0 &&
      !iree_string_view_is_empty(match.first_unexpected_domain)) {
    return iree_make_status(
        IREE_STATUS_FAILED_PRECONDITION,
        "wrong diagnostic: expected %.*s, received %.*s/%03u",
        (int)expected_diagnostics.size, expected_diagnostics.data,
        (int)match.first_unexpected_domain.size,
        match.first_unexpected_domain.data,
        (unsigned)match.first_unexpected_code);
  }
  if (match.match_count == 0) {
    return iree_make_status(
        IREE_STATUS_FAILED_PRECONDITION,
        "missing diagnostic: compiler exited %d but the compile report "
        "contained no error diagnostics; expected %.*s",
        compiler_exit_code, (int)expected_diagnostics.size,
        expected_diagnostics.data);
  }
  return iree_ok_status();
}
