# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

from __future__ import annotations

import pytest

from loom.reporting.compile_report import CompileReportError, parse_compile_report
from loom.reporting.compile_report_expansions import suggest_expansions
from loom.reporting.compile_report_view import (
    build_compile_report_show,
    format_compile_report_show_text,
)


def _report(*, mode: str = "details") -> dict[str, object]:
    return {
        "kind": "loom.compile_report",
        "schema_version": 0,
        "mode": mode,
        "status": {"code": 0, "name": "OK"},
        "entries": {"count": 0, "rows": []},
    }


def _selection(count: int, *, index: int = 0) -> dict[str, object]:
    return {
        "index": index,
        "function": "multiply",
        "source_op": "scalar.mulf",
        "selection": "rule",
        "plan_key": "exact_binary32",
        "emitted_low_op_count": count,
    }


@pytest.mark.parametrize(
    "operation",
    [
        "scalar.mulf",
        "scalar.divf",
        "scalar.fptrunc",
        "vector.dotf",
        "index.div",
    ],
)
def test_scalar_vector_conversion_and_index_costs_need_no_scalarization_marker(
    operation: str,
) -> None:
    report = _report()
    row = _selection(163)
    row["source_op"] = operation
    report["source_low"] = {"count": 1, "rows": [row]}
    document = parse_compile_report(report)
    show = build_compile_report_show(document)
    assert "scalarization" not in show
    assert show["expansions"]["rows"] == [
        {
            "severity": "warning",
            "stage": "source_to_low",
            "evidence_path": "source_low.rows[0].emitted_low_op_count",
            "maximum_op_count": 163,
            "unit": "Low operations",
            "source_op_count": 1,
            "function": "multiply",
            "source_op": operation,
            "recipe": "exact_binary32",
        }
    ]
    text = format_compile_report_show_text(show)
    assert f"WARNING: multiply {operation}: up to 163 Low operations" in text
    assert "not final instructions or runtime" in text
    (suggestion,) = suggest_expansions(document)
    assert suggestion.suggestion_id == "lowering.reduce_expansion"
    assert suggestion.evidence[0].value == 163
    assert "preserves the source semantics" in suggestion.action


def test_summary_peak_cannot_be_hidden_by_a_cheap_average() -> None:
    report = _report(mode="summary")
    row = _selection(262)
    row.update(selected_op_count=100, maximum_emitted_low_op_count=163)
    report["source_low"] = {"selection_summaries": {"count": 1, "rows": [row]}}
    document = parse_compile_report(report)
    (warning,) = build_compile_report_show(document)["expansions"]["rows"]
    assert warning["maximum_op_count"] == 163
    assert warning["source_op_count"] == 100
    assert warning["evidence_path"].endswith("maximum_emitted_low_op_count")


def test_threshold_is_per_operation_not_total_or_recipe_name() -> None:
    report = _report(mode="summary")
    rows = [_selection(500), _selection(6, index=1)]
    rows[0].update(selected_op_count=100, maximum_emitted_low_op_count=5)
    rows[1].update(
        selected_op_count=1,
        maximum_emitted_low_op_count=6,
        plan_key="approximate_binary32_reciprocal_multiply",
    )
    report["source_low"] = {"selection_summaries": {"count": 2, "rows": rows}}
    (warning,) = parse_compile_report(report).expansion_inventory.warnings
    assert warning["maximum_op_count"] == 6
    assert warning["recipe"] == "approximate_binary32_reciprocal_multiply"


def test_summary_includes_lowerings_without_recipe_or_descriptor_keys() -> None:
    report = _report(mode="summary")
    report["source_low"] = {
        "selected_op_count": 2,
        "unkeyed_maximum_emitted_op_count": 97,
    }
    document = parse_compile_report(report)
    (warning,) = document.expansion_inventory.warnings
    assert warning["maximum_op_count"] == 97
    assert warning["source_op_count"] is None
    assert document.expansion_inventory.missing_evidence == ()
    (suggestion,) = suggest_expansions(document)
    assert "--compile-report=details" in suggestion.action


@pytest.mark.parametrize("mode", ["summary", "details"])
def test_math_recipe_growth_is_visible_before_source_to_low(mode: str) -> None:
    report = _report(mode=mode)
    section = {"rewritten_op_count": 1, "maximum_created_op_count": 40, "count": 0}
    if mode == "details":
        section.update(
            count=1,
            rows=[
                {
                    "index": 0,
                    "function": "activate",
                    "source_op": "vector.geluf",
                    "action": "rewritten",
                    "recipe": "gelu-tanh",
                    "created_op_count": 40,
                }
            ],
        )
    report["math_legalization"] = section
    document = parse_compile_report(report)
    (warning,) = document.expansion_inventory.warnings
    assert warning["stage"] == "math_legalization"
    assert warning["maximum_op_count"] == 40
    assert warning["unit"] == "source IR operations"
    (suggestion,) = suggest_expansions(document)
    assert ("--compile-report=details" in suggestion.action) == (mode == "summary")


def test_missing_peak_is_unavailable_not_an_average_based_clean_bill() -> None:
    report = _report(mode="summary")
    row = _selection(262)
    row["selected_op_count"] = 100
    report["source_low"] = {"selection_summaries": {"count": 1, "rows": [row]}}
    document = parse_compile_report(report)
    show = build_compile_report_show(document)
    assert show["expansions"]["rows"] == []
    assert show["expansions"]["missing_evidence"] == [
        "source_low.selection_summaries.rows[0].maximum_emitted_low_op_count"
    ]
    assert "Expansion evidence incomplete" in format_compile_report_show_text(show)
    (suggestion,) = suggest_expansions(document)
    assert suggestion.suggestion_id == "lowering.inspect_expansion"


def test_missing_counts_are_distinct_from_a_compact_native_lowering() -> None:
    report = _report()
    row = _selection(1)
    report["source_low"] = {"count": 1, "rows": [row]}
    assert "expansions" not in build_compile_report_show(parse_compile_report(report))
    del row["emitted_low_op_count"]
    assert parse_compile_report(report).expansion_inventory.missing_evidence


@pytest.mark.parametrize("count", [True, "163", None])
def test_malformed_counts_fail_at_the_report_boundary(count: object) -> None:
    report = _report()
    row = _selection(163)
    row["emitted_low_op_count"] = count
    report["source_low"] = {"count": 1, "rows": [row]}
    with pytest.raises(CompileReportError, match="emitted_low_op_count"):
        parse_compile_report(report)
