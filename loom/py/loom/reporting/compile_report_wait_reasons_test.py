# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

from __future__ import annotations

from copy import deepcopy

import pytest

from loom.reporting.compile_report import CompileReportError, parse_compile_report
from loom.reporting.compile_report_view import (
    build_compile_report_diff,
    build_compile_report_show,
    format_compile_report_diff_text,
    format_compile_report_show_text,
)


def _summary(
    *,
    actions: int = 1,
    explicit: int = 0,
    planned: int | None = None,
    full: int = 1,
    partial: int | None = None,
    drained: int = 1,
    max_drained: int = 1,
    max_outstanding: int = 1,
    max_full_outstanding: int | None = None,
) -> dict[str, int]:
    if planned is None:
        planned = actions - explicit
    if partial is None:
        partial = actions - full
    if max_full_outstanding is None:
        max_full_outstanding = max_outstanding if full else 0
    return {
        "action_count": actions,
        "explicit_action_count": explicit,
        "planned_action_count": planned,
        "full_drain_count": full,
        "partial_wait_count": partial,
        "drained_count": drained,
        "max_drained_count": max_drained,
        "max_outstanding_before": max_outstanding,
        "max_full_drain_outstanding_before": max_full_outstanding,
    }


def _reason(
    function: str,
    counter: str,
    counter_id: int,
    reason: str,
    reason_id: int,
    summary: dict[str, int] | None = None,
) -> dict[str, object]:
    return {
        "function": function,
        "counter": counter,
        "counter_id": counter_id,
        "reason": reason,
        "reason_id": reason_id,
        "summary": summary or _summary(),
    }


def _aggregate(rows: list[dict[str, object]]) -> dict[str, int]:
    summaries = [row["summary"] for row in rows]
    if not summaries:
        return {
            "action_count": 0,
            "explicit_action_count": 0,
            "planned_action_count": 0,
            "full_drain_count": 0,
            "partial_wait_count": 0,
            "drained_count": 0,
            "max_drained_count": 0,
            "max_outstanding_before": 0,
            "max_full_drain_outstanding_before": 0,
        }
    return {
        field: (
            max(summary[field] for summary in summaries)
            if field.startswith("max_")
            else sum(summary[field] for summary in summaries)
        )
        for field in summaries[0]
    }


def _report(
    rows: list[dict[str, object]],
    *,
    functions: tuple[str, ...] = ("stream",),
    mode: str = "summary",
    include_reasons: bool = True,
) -> dict[str, object]:
    indexed_rows = [dict(row, index=index) for index, row in enumerate(rows)]
    entries = []
    for index, function in enumerate(functions):
        function_rows = [row for row in rows if row["function"] == function]
        entries.append(
            {
                "index": index,
                "function": function,
                "source_function": function,
                "wait_plan": _aggregate(function_rows),
            }
        )
    report: dict[str, object] = {
        "kind": "loom.compile_report",
        "schema_version": 0,
        "mode": mode,
        "status": {"code": 0, "name": "OK"},
        "entries": {"count": len(entries), "rows": entries},
    }
    if include_reasons:
        report["wait_reason_summary_rows"] = {
            "count": len(indexed_rows),
            "rows": indexed_rows,
        }
    return report


def _document(report: dict[str, object], source: str = "report.json"):
    return parse_compile_report(report, source=source)


@pytest.mark.parametrize("mode", ["summary", "details"])
def test_show_groups_wait_reasons_by_entry_in_both_report_modes(mode: str) -> None:
    report = _report(
        [
            _reason(
                "stream",
                "vmem_load",
                1,
                "amdgpu.ssa_use",
                2,
                _summary(
                    actions=5,
                    full=4,
                    drained=6,
                    max_drained=2,
                    max_outstanding=2,
                ),
            ),
            _reason(
                "stream",
                "smem",
                4,
                "amdgpu.loop_entry_conservative_ssa_use",
                13,
                _summary(),
            ),
        ],
        mode=mode,
    )

    show = build_compile_report_show(_document(report))
    wait_reasons = show["entries"][0]["wait_reasons"]
    assert wait_reasons["reason_count"] == 2
    assert [(row["counter"], row["reason"]) for row in wait_reasons["reasons"]] == [
        ("smem", "amdgpu.loop_entry_conservative_ssa_use"),
        ("vmem_load", "amdgpu.ssa_use"),
    ]
    assert "counter_id" not in wait_reasons["reasons"][0]
    text = format_compile_report_show_text(show)
    assert "Wait reasons (compiler analysis)" in text
    assert "5 actions (0 explicit, 5 planned)" in text
    assert (
        "1 action (0 explicit, 1 planned); 1 full drain, 0 partial waits; "
        "1 packet drained"
    ) in text
    assert "2 block-local outstanding" in text


def test_diff_exposes_reason_shift_with_unchanged_total_action_count() -> None:
    baseline = _report(
        [
            _reason(
                "stream",
                "vmem_load",
                1,
                "amdgpu.ssa_use",
                2,
                _summary(
                    actions=5,
                    full=4,
                    drained=6,
                    max_drained=2,
                    max_outstanding=2,
                ),
            ),
            _reason(
                "stream",
                "smem",
                4,
                "amdgpu.loop_entry_conservative_ssa_use",
                13,
                _summary(actions=4, full=4, drained=0, max_drained=0),
            ),
        ]
    )
    candidate = _report(
        [
            _reason(
                "stream",
                "vmem_load",
                1,
                "amdgpu.ssa_use",
                2,
                _summary(
                    actions=4,
                    full=2,
                    drained=4,
                    max_outstanding=4,
                    max_full_outstanding=1,
                ),
            ),
            _reason(
                "stream",
                "vmem_load",
                1,
                "amdgpu.loop_carried_derived_ssa_use",
                14,
                _summary(
                    actions=2,
                    full=0,
                    drained=2,
                    max_outstanding=4,
                ),
            ),
            _reason(
                "stream",
                "smem",
                4,
                "amdgpu.loop_entry_conservative_ssa_use",
                13,
                _summary(actions=3, full=3, drained=0, max_drained=0),
            ),
        ]
    )
    assert baseline["entries"]["rows"][0]["wait_plan"]["action_count"] == 9
    assert candidate["entries"]["rows"][0]["wait_plan"]["action_count"] == 9

    diff = build_compile_report_diff(
        _document(baseline, "baseline.json"),
        _document(candidate, "candidate.json"),
    )
    wait_reasons = diff["entries"][0]["wait_reasons"]
    assert wait_reasons["changed_reason_count"] == 3
    assert wait_reasons["unchanged_reason_count"] == 0
    by_reason = {row["reason"]: row for row in wait_reasons["reasons"]}
    assert by_reason["amdgpu.loop_carried_derived_ssa_use"]["status"] == "added"
    assert by_reason["amdgpu.ssa_use"]["changes"]["full_drain_count"] == {
        "baseline": 4,
        "candidate": 2,
        "delta": -2,
        "change_percent": -50.0,
    }
    text = format_compile_report_diff_text(diff)
    assert "3 changed, 0 unchanged" in text
    assert "amdgpu.loop_carried_derived_ssa_use: added" in text
    assert "full drains: 4 -> 2, delta -2" in text


def test_reason_redistribution_marks_entry_changed_when_aggregates_match() -> None:
    full = _summary(full=1)
    partial = _summary(full=0)
    baseline = _report(
        [
            _reason("stream", "vmem_load", 1, "reason.a", 1, full),
            _reason("stream", "vmem_load", 1, "reason.b", 2, partial),
        ]
    )
    candidate = _report(
        [
            _reason("stream", "vmem_load", 1, "reason.a", 1, partial),
            _reason("stream", "vmem_load", 1, "reason.b", 2, full),
        ]
    )
    assert (
        baseline["entries"]["rows"][0]["wait_plan"]
        == candidate["entries"]["rows"][0]["wait_plan"]
    )

    diff = build_compile_report_diff(_document(baseline), _document(candidate))
    assert diff["changed_entry_count"] == 1
    assert diff["entries"][0]["compiler_analysis"]["changed"] == {}
    assert diff["entries"][0]["wait_reasons"]["changed_reason_count"] == 2


def test_multi_entry_diff_keeps_reason_rows_with_their_owning_function() -> None:
    baseline = _report(
        [
            _reason("first", "vmem_load", 1, "reason.first", 1),
            _reason("second", "smem", 4, "reason.second", 2),
        ],
        functions=("first", "second"),
    )
    candidate = deepcopy(baseline)
    candidate["wait_reason_summary_rows"]["rows"][0]["summary"] = _summary(full=0)
    candidate["entries"]["rows"][0]["wait_plan"] = _summary(full=0)

    show = build_compile_report_show(_document(baseline))
    assert [
        entry["wait_reasons"]["reasons"][0]["reason"] for entry in show["entries"]
    ] == ["reason.first", "reason.second"]
    diff = build_compile_report_diff(_document(baseline), _document(candidate))
    assert diff["changed_entry_count"] == 1
    assert diff["unchanged_entry_count"] == 1
    assert diff["entries"][0]["identity"]["name"] == "first"
    assert diff["entries"][0]["wait_reasons"]["changed_reason_count"] == 1


def test_diff_preserves_missing_reason_evidence_as_unavailable() -> None:
    rows = [_reason("stream", "vmem_load", 1, "reason", 1)]
    baseline = _report(rows, include_reasons=False)
    candidate = _report(rows)

    diff = build_compile_report_diff(_document(baseline), _document(candidate))
    assert diff["entries"][0]["wait_reasons"] == {
        "availability": {"baseline": "unavailable", "candidate": "available"}
    }


def test_diff_handles_available_empty_and_added_reasons() -> None:
    baseline = _report([])
    candidate = _report([_reason("stream", "vmem_load", 1, "reason", 1)])

    diff = build_compile_report_diff(_document(baseline), _document(candidate))
    wait_reasons = diff["entries"][0]["wait_reasons"]
    assert wait_reasons["changed_reason_count"] == 1
    assert wait_reasons["reasons"][0]["status"] == "added"


def test_forced_entry_pair_compares_reason_identity_across_function_renames() -> None:
    baseline = _report(
        [_reason("before", "vmem_load", 1, "reason", 1)], functions=("before",)
    )
    candidate = _report(
        [_reason("after", "vmem_load", 1, "reason", 1)], functions=("after",)
    )

    diff = build_compile_report_diff(
        _document(baseline), _document(candidate), force=True
    )
    assert diff["changed_entry_count"] == 0
    assert diff["unchanged_entry_count"] == 1


def _duplicate_reason(report: dict[str, object]) -> None:
    rows = report["wait_reason_summary_rows"]["rows"]
    rows.append(dict(rows[0], index=1))
    report["wait_reason_summary_rows"]["count"] = 2


def _inconsistent_counter_id(report: dict[str, object]) -> None:
    rows = report["wait_reason_summary_rows"]["rows"]
    rows.append(
        dict(
            rows[0],
            index=1,
            counter_id=2,
            reason="another_reason",
            reason_id=2,
        )
    )
    report["wait_reason_summary_rows"]["count"] = 2


def _full_drain_maximum_without_full_drain(report: dict[str, object]) -> None:
    summary = report["wait_reason_summary_rows"]["rows"][0]["summary"]
    summary["full_drain_count"] = 0
    summary["partial_wait_count"] = 1


@pytest.mark.parametrize(
    ("mutation", "message"),
    [
        (
            lambda report: report["wait_reason_summary_rows"].__setitem__("count", 2),
            "count: expected 1, got 2",
        ),
        (
            lambda report: report["wait_reason_summary_rows"].pop("rows"),
            "rows: missing 1 rows",
        ),
        (
            lambda report: report["wait_reason_summary_rows"]["rows"][0].__setitem__(
                "index", 1
            ),
            "index: expected 0, got 1",
        ),
        (
            lambda report: report["wait_reason_summary_rows"]["rows"][0].__setitem__(
                "counter", ""
            ),
            "counter: expected non-empty string",
        ),
        (
            lambda report: report["wait_reason_summary_rows"]["rows"][0].__setitem__(
                "counter_id", False
            ),
            "counter_id: expected non-negative integer",
        ),
        (
            lambda report: report["wait_reason_summary_rows"]["rows"][0][
                "summary"
            ].__setitem__("action_count", 0),
            "action_count: expected a positive count",
        ),
        (
            lambda report: report["wait_reason_summary_rows"]["rows"][0][
                "summary"
            ].__setitem__("planned_action_count", 0),
            "action count must equal explicit plus planned",
        ),
        (
            lambda report: report["wait_reason_summary_rows"]["rows"][0][
                "summary"
            ].__setitem__("partial_wait_count", 1),
            "action count must equal full drains plus partial waits",
        ),
        (
            lambda report: report["wait_reason_summary_rows"]["rows"][0][
                "summary"
            ].__setitem__("max_drained_count", 2),
            "max_drained_count: exceeds total drained count",
        ),
        (
            lambda report: report["wait_reason_summary_rows"]["rows"][0][
                "summary"
            ].__setitem__("max_outstanding_before", 0),
            "max_drained_count: exceeds maximum outstanding count",
        ),
        (
            lambda report: report["wait_reason_summary_rows"]["rows"][0][
                "summary"
            ].__setitem__("max_full_drain_outstanding_before", 2),
            "max_full_drain_outstanding_before: exceeds maximum outstanding",
        ),
        (
            _full_drain_maximum_without_full_drain,
            "max_full_drain_outstanding_before: requires a full drain",
        ),
        (_duplicate_reason, "duplicate wait reason"),
        (_inconsistent_counter_id, "inconsistent stable name/id mapping"),
        (
            lambda report: report["wait_reason_summary_rows"]["rows"][0].__setitem__(
                "function", "missing"
            ),
            "unknown function 'missing'",
        ),
    ],
)
def test_rejects_malformed_wait_reason_evidence(mutation, message: str) -> None:
    report = _report([_reason("stream", "vmem_load", 1, "reason", 1)])
    mutation(report)
    with pytest.raises(CompileReportError, match=message):
        _document(report)
