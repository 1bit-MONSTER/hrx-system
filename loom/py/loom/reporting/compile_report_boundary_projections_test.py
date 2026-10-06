# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

from __future__ import annotations

from copy import deepcopy

import pytest

from loom.reporting.compile_report import CompileReportError, parse_compile_report
from loom.reporting.compile_report_boundary_projections import (
    build_boundary_projection_show,
    suggest_boundary_projections,
)
from loom.reporting.compile_report_view import (
    build_compile_report_show,
    format_compile_report_show_text,
)


def _row(
    function: str,
    outcome: str,
    reason: str,
    shape: list[int],
    *,
    prefix_rank: int = 0,
    component_count: int = 0,
    source_value: int = 0,
) -> dict[str, object]:
    row: dict[str, object] = {
        "index": 0,
        "function": function,
        "source_op": "scf.for",
        "source_op_kind": 1280,
        "projection": "loop-vector-bank",
        "boundary": "loop_state",
        "outcome": outcome,
        "reason": reason,
        "operation": 0,
        "source_value": source_value,
        "source_type_kind": 9,
        "source_type": "vector",
        "source_element_type": 12,
        "source_element": "f32",
        "source_rank": len(shape),
        "source_shape": shape,
        "projected_prefix_rank": prefix_rank,
        "component_count": component_count,
    }
    if component_count:
        row["component_shape"] = shape[prefix_rank:]
    return row


def _report(*, details: bool = True) -> dict[str, object]:
    rows = [
        _row(
            "gdn",
            "selected",
            "static_component_accesses",
            [4, 4],
            prefix_rank=2,
            component_count=16,
        ),
        _row("dense", "preserved", "whole_value_use", [8]),
        _row(
            "dynamic_bank",
            "rejected",
            "non_static_component_access",
            [-1],
        ),
        _row(
            "mixed_bank",
            "rejected",
            "inconsistent_component_access",
            [2, 2],
            prefix_rank=1,
            component_count=2,
        ),
        _row(
            "whole_bank",
            "rejected",
            "unsupported_value_use",
            [2],
            prefix_rank=1,
            component_count=2,
        ),
        _row(
            "large_bank",
            "rejected",
            "component_count_limit",
            [256, 256],
            prefix_rank=2,
        ),
        _row(
            "peer_bank",
            "rejected",
            "peer_bank_rejected",
            [2],
            prefix_rank=1,
            component_count=2,
        ),
        _row(
            "transport_bank",
            "rejected",
            "boundary_projection_rejected",
            [2],
            prefix_rank=1,
            component_count=2,
        ),
        _row(
            "named_bank",
            "rejected",
            "value_metadata_use",
            [2],
            prefix_rank=1,
            component_count=2,
        ),
    ]
    for index, row in enumerate(rows):
        row["index"] = index
    projections: dict[str, object] = {
        "count": len(rows),
        "selected_count": 1,
        "preserved_count": 1,
        "rejected_count": 7,
    }
    if details:
        projections["rows"] = rows
    return {
        "kind": "loom.compile_report",
        "schema_version": 0,
        "mode": "details" if details else "summary",
        "status": {"code": 0, "name": "OK"},
        "entries": {
            "count": 1,
            "rows": [{"index": 0, "function": "gdn", "source_function": "gdn"}],
        },
        "source_low": {
            "count": 0,
            **({"rows": []} if details else {}),
            "boundary_projections": projections,
        },
    }


def test_show_explains_selected_preserved_and_rejected_representations() -> None:
    document = parse_compile_report(_report())
    projection_view = build_boundary_projection_show(document)
    assert projection_view is not None
    assert projection_view["selected_count"] == 1
    selected, preserved, dynamic, mixed, *_ = projection_view["rows"]
    assert selected["source_type_text"] == "vector<4x4xf32>"
    assert selected["component_type_text"] == "f32"
    assert preserved["source_type_text"] == "vector<8xf32>"
    assert "component_type_text" not in preserved
    assert dynamic["source_type_text"] == "vector<?xf32>"
    assert mixed["component_type_text"] == "vector<2xf32>"

    show = build_compile_report_show(document)
    assert show["boundary_projections"] == projection_view
    text = format_compile_report_show_text(show)
    assert "Source boundary projections" in text
    assert "selected=1 preserved=1 rejected=7" in text
    assert (
        "gdn scf.for[0] loop_state[0]: loop-vector-bank selected "
        "vector<4x4xf32> -> 16 x f32 reason=static_component_accesses" in text
    )
    assert "dense scf.for[0] loop_state[0]" in text
    assert "preserved vector<8xf32> reason=whole_value_use" in text


def test_summary_keeps_decision_counts_without_claiming_row_details() -> None:
    document = parse_compile_report(_report(details=False))
    view = build_boundary_projection_show(document)
    assert view == {
        "count": 9,
        "selected_count": 1,
        "preserved_count": 1,
        "rejected_count": 7,
    }
    assert suggest_boundary_projections(document) == ()
    text = format_compile_report_show_text(build_compile_report_show(document))
    assert "selected=1 preserved=1 rejected=7" in text
    assert "gdn scf.for" not in text


def test_suggestions_cover_only_rejections_with_concrete_source_experiments() -> None:
    suggestions = suggest_boundary_projections(parse_compile_report(_report()))
    assert [suggestion.suggestion_id for suggestion in suggestions] == [
        "vector.compare_static_bank_access",
        "vector.compare_uniform_bank_components",
        "vector.compare_componentwise_bank_state",
    ]
    assert [suggestion.entry_name for suggestion in suggestions] == [
        "dynamic_bank",
        "mixed_bank",
        "whole_bank",
    ]
    for suggestion in suggestions:
        assert "Selection enables an experiment" in suggestion.action
        assert "does not by itself establish a performance gain" in suggestion.action
        evidence = {fact.path: fact.value for fact in suggestion.evidence}
        assert any(path.endswith(".reason") for path in evidence)
        assert any(path.endswith(".source_shape") for path in evidence)


def test_failed_compilation_and_missing_section_have_no_projection_advice() -> None:
    report = _report()
    report["status"] = {"code": 9, "name": "FAILED_PRECONDITION"}
    assert suggest_boundary_projections(parse_compile_report(report)) == ()
    del report["source_low"]["boundary_projections"]
    document = parse_compile_report(report)
    assert build_boundary_projection_show(document) is None
    assert "boundary_projections" not in build_compile_report_show(document)


@pytest.mark.parametrize(
    "corruption",
    [
        "summary_rows",
        "details_without_rows",
        "total_count",
        "outcome_sum",
        "outcome_rows",
        "row_index",
        "duplicate",
        "unknown_outcome",
        "rank",
        "zero_extent",
        "boolean_extent",
        "prefix_rank",
        "missing_component_shape",
        "unexpected_component_shape",
        "component_suffix",
        "component_count",
        "selected_without_components",
    ],
)
def test_rejects_inconsistent_projection_records(corruption: str) -> None:
    report = deepcopy(_report())
    projections = report["source_low"]["boundary_projections"]
    rows = projections["rows"]
    if corruption == "summary_rows":
        report["mode"] = "summary"
    elif corruption == "details_without_rows":
        del projections["rows"]
    elif corruption == "total_count":
        projections["count"] += 1
    elif corruption == "outcome_sum":
        projections["rejected_count"] -= 1
    elif corruption == "outcome_rows":
        rows[1]["outcome"] = "rejected"
    elif corruption == "row_index":
        rows[0]["index"] = 3
    elif corruption == "duplicate":
        rows[1].update(
            {
                key: rows[0][key]
                for key in (
                    "function",
                    "operation",
                    "source_value",
                    "projection",
                    "boundary",
                )
            }
        )
    elif corruption == "unknown_outcome":
        rows[2]["outcome"] = "deferred"
    elif corruption == "rank":
        rows[0]["source_rank"] = 3
    elif corruption == "zero_extent":
        rows[0]["source_shape"][0] = 0
    elif corruption == "boolean_extent":
        rows[0]["source_shape"][0] = True
    elif corruption == "prefix_rank":
        rows[0]["projected_prefix_rank"] = 3
    elif corruption == "missing_component_shape":
        del rows[0]["component_shape"]
    elif corruption == "unexpected_component_shape":
        rows[1]["component_shape"] = []
    elif corruption == "component_suffix":
        rows[3]["component_shape"] = [4]
    elif corruption == "component_count":
        rows[0]["component_count"] = 15
    else:
        rows[0]["component_count"] = 0
        del rows[0]["component_shape"]
    with pytest.raises(CompileReportError, match="boundary_projections"):
        build_boundary_projection_show(parse_compile_report(report))
