// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/build_tools/corpus/diagnostic_report.h"

#include <string>

#include "iree/base/status_cc.h"
#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"

namespace loom {
namespace {

using ::testing::HasSubstr;

static const loom_corpus_diagnostic_id_t kTarget072 = {
    /*.domain=*/LOOM_ERROR_DOMAIN_TARGET,
    /*.code=*/72,
};

static std::string TakeStatusString(iree_status_t status) {
  std::string text = iree::Status::ToString(status);
  iree_status_free(status);
  return text;
}

TEST(DiagnosticIdTest, ParsesCanonicalIdentity) {
  loom_corpus_diagnostic_id_t diagnostic_id = {};
  IREE_ASSERT_OK(
      loom_corpus_diagnostic_id_parse(IREE_SV("TARGET/072"), &diagnostic_id));
  EXPECT_EQ(diagnostic_id.domain, LOOM_ERROR_DOMAIN_TARGET);
  EXPECT_EQ(diagnostic_id.code, 72);
}

TEST(DiagnosticIdTest, RejectsNonCanonicalIdentity) {
  loom_corpus_diagnostic_id_t diagnostic_id = {};
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_INVALID_ARGUMENT,
      loom_corpus_diagnostic_id_parse(IREE_SV("TARGET/72"), &diagnostic_id));
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_INVALID_ARGUMENT,
      loom_corpus_diagnostic_id_parse(IREE_SV("UNKNOWN/001"), &diagnostic_id));
}

TEST(DiagnosticReportTest, AcceptsOnlyExpectedErrorDiagnostics) {
  IREE_EXPECT_OK(loom_corpus_compile_report_expect_diagnostic(1, IREE_SV(R"({
        "kind": "loom.compile_report",
        "mode": "details",
        "diagnostics": [
          {"severity": "warning"},
          {"severity": "error", "domain": "TARGET", "code": 72},
          {"severity": "error", "domain": "TARGET", "code": 72}
        ]
      })"),
                                                              kTarget072));
}

TEST(DiagnosticReportTest, AcceptsExpectedDiagnosticAlongsideOtherErrors) {
  IREE_EXPECT_OK(loom_corpus_compile_report_expect_diagnostic(1, IREE_SV(R"({
        "kind": "loom.compile_report",
        "mode": "details",
        "diagnostics": [
          {"severity": "error", "domain": "TARGET", "code": 72},
          {"severity": "error", "domain": "TYPE", "code": 1}
        ]
      })"),
                                                              kTarget072));
}

TEST(DiagnosticReportTest, DetectsXpass) {
  iree_status_t status = loom_corpus_compile_report_expect_diagnostic(
      0, iree_string_view_empty(), kTarget072);
  EXPECT_EQ(iree_status_code(status), IREE_STATUS_FAILED_PRECONDITION);
  EXPECT_THAT(TakeStatusString(status), HasSubstr("XPASS"));
}

TEST(DiagnosticReportTest, DetectsWrongDiagnostic) {
  iree_status_t status =
      loom_corpus_compile_report_expect_diagnostic(1, IREE_SV(R"({
        "kind": "loom.compile_report",
        "mode": "details",
        "diagnostics": [
          {"severity": "error", "domain": "TYPE", "code": 1}
        ]
      })"),
                                                   kTarget072);
  EXPECT_EQ(iree_status_code(status), IREE_STATUS_FAILED_PRECONDITION);
  const std::string message = TakeStatusString(status);
  EXPECT_THAT(message, HasSubstr("wrong diagnostic"));
  EXPECT_THAT(message, HasSubstr("TYPE/001"));
}

TEST(DiagnosticReportTest, DetectsMissingDiagnostic) {
  iree_status_t status =
      loom_corpus_compile_report_expect_diagnostic(1, IREE_SV(R"({
        "kind": "loom.compile_report",
        "mode": "details"
      })"),
                                                   kTarget072);
  EXPECT_EQ(iree_status_code(status), IREE_STATUS_FAILED_PRECONDITION);
  EXPECT_THAT(TakeStatusString(status), HasSubstr("missing diagnostic"));
}

TEST(DiagnosticReportTest, DetectsMissingStructuredReport) {
  iree_status_t status = loom_corpus_compile_report_expect_diagnostic(
      1, iree_string_view_empty(), kTarget072);
  EXPECT_EQ(iree_status_code(status), IREE_STATUS_FAILED_PRECONDITION);
  EXPECT_THAT(TakeStatusString(status),
              HasSubstr("without a structured compile report"));
}

}  // namespace
}  // namespace loom
