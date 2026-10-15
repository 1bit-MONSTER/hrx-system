# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""X86 compile report suggestion provider."""

from __future__ import annotations

from loom.reporting.compile_report import CompileReportDocument
from loom.reporting.compile_report_suggestions import (
    CompileReportSuggestionOptions,
    CompileReportSuggestionResult,
)


class X86CompileReportSuggestionProvider:
    """Recognizes x86 reports for shared suggestion handling."""

    target_family = "x86"
    provider_name = "x86"

    def suggest(
        self,
        document: CompileReportDocument,
        options: CompileReportSuggestionOptions | None = None,
    ) -> CompileReportSuggestionResult:
        del document, options
        return CompileReportSuggestionResult(
            provider_name=self.provider_name,
            unavailable_reason=None,
        )


X86_COMPILE_REPORT_SUGGESTION_PROVIDER = X86CompileReportSuggestionProvider()
