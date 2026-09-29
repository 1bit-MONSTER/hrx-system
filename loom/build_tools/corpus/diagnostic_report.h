// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Structured diagnostic matching for corpus compilation xfails.

#ifndef LOOM_BUILD_TOOLS_CORPUS_DIAGNOSTIC_REPORT_H_
#define LOOM_BUILD_TOOLS_CORPUS_DIAGNOSTIC_REPORT_H_

#include "iree/base/api.h"
#include "loom/error/error_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

// Stable identity of one expected compiler diagnostic.
typedef struct loom_corpus_diagnostic_id_t {
  // Semantic diagnostic domain.
  loom_error_domain_t domain;
  // Stable code within |domain|.
  uint16_t code;
} loom_corpus_diagnostic_id_t;

// Parses a canonical DOMAIN/NNN diagnostic identity.
iree_status_t loom_corpus_diagnostic_id_parse(
    iree_string_view_t value, loom_corpus_diagnostic_id_t* out_diagnostic_id);

// Verifies one compiler result against its expected diagnostic identity.
//
// A zero exit code is an XPASS. A nonzero exit code succeeds only when
// |compile_report_json| is a details-mode Loom compile report containing
// |expected_diagnostic_id|. Additional errors may describe other unsupported
// operations in the same root. Missing reports, reports with no error
// diagnostic, and reports without the expected identity fail distinctly.
iree_status_t loom_corpus_compile_report_expect_diagnostic(
    int compiler_exit_code, iree_string_view_t compile_report_json,
    loom_corpus_diagnostic_id_t expected_diagnostic_id);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_BUILD_TOOLS_CORPUS_DIAGNOSTIC_REPORT_H_
