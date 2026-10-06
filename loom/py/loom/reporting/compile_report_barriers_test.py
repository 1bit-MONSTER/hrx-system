# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 WITH LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

from __future__ import annotations

from copy import deepcopy

import pytest

from loom.reporting.compile_report import CompileReportError, parse_compile_report
from loom.reporting.compile_report_barriers import build_barrier_show
from loom.reporting.compile_report_view import (
    build_compile_report_show,
    format_compile_report_show_text,
)


def _selection(index: int, operation: str, **fields: object) -> dict:
    row = {
        "index": index,
        "function": "reuse",
        "source_op": operation,
        "selection": "plan",
        "plan_key": f"target.{operation}",
        "selected_op_count": 1,
        "emitted_low_op_count": 1,
    }
    row.update(fields)
    return row


def _report() -> dict:
    return {
        "kind": "loom.compile_report",
        "schema_version": 0,
        "mode": "summary",
        "status": {"code": 0, "name": "OK"},
        "entries": {
            "count": 1,
            "rows": [{"index": 0, "function": "reuse", "code_byte_count": 64}],
        },
        "source_low": {
            "selection_summaries": {
                "count": 4,
                "rows": [
                    _selection(
                        0,
                        "kernel.barrier",
                        plan_key="target.barrier.complete",
                        emitted_low_op_count=2,
                        exact_dynamic_op_count=1,
                        unknown_dynamic_op_count=0,
                        dynamic_selected_op_count=4,
                        dynamic_emitted_low_op_count=8,
                    ),
                    _selection(
                        1,
                        "kernel.barrier.arrive",
                        plan_key="target.barrier.arrive",
                        unknown_dynamic_op_count=1,
                    ),
                    _selection(
                        2,
                        "kernel.barrier.wait",
                        selection="rule",
                        plan_key=None,
                        descriptor_key="target.barrier.wait",
                        descriptor_semantic_tag="barrier.wait",
                    ),
                    _selection(3, "scalar.addi"),
                ],
            }
        },
    }


def test_show_separates_authored_phases_from_target_realization() -> None:
    document = parse_compile_report(_report())
    barriers = build_barrier_show(document)
    assert barriers is not None
    assert barriers["count"] == 3
    complete, arrive, wait = barriers["rows"]
    assert [(row["phase"], row["source_op"]) for row in barriers["rows"]] == [
        ("complete", "kernel.barrier"),
        ("arrive", "kernel.barrier.arrive"),
        ("wait", "kernel.barrier.wait"),
    ]
    assert complete["plan_key"] == "target.barrier.complete"
    assert complete["emitted_low_op_count"] == 2
    assert complete["dynamic"] == {
        "exact_source_op_count": 1,
        "unknown_source_op_count": 0,
        "selected_op_count": 4,
        "emitted_low_op_count": 8,
    }
    assert arrive["dynamic"]["unknown_source_op_count"] == 1
    assert wait["selection"] == "rule"
    assert wait["descriptor_key"] == "target.barrier.wait"
    assert "plan_key" not in wait

    show = build_compile_report_show(document)
    assert show["barriers"] == barriers
    text = format_compile_report_show_text(show)
    assert "Barrier realization (compiler analysis)" in text
    assert (
        "reuse complete (kernel.barrier): selection=plan "
        "plan=target.barrier.complete static=1 source/2 low "
        "dynamic=4 source/8 low"
    ) in text
    assert (
        "reuse arrive (kernel.barrier.arrive): selection=plan "
        "plan=target.barrier.arrive static=1 source/1 low "
        "dynamic=unknown (1 source op)"
    ) in text
    assert "descriptor=target.barrier.wait semantic=barrier.wait" in text


def test_show_omits_barrier_section_without_barrier_summaries() -> None:
    report = _report()
    report["source_low"]["selection_summaries"]["rows"] = [
        report["source_low"]["selection_summaries"]["rows"][-1]
    ]
    report["source_low"]["selection_summaries"]["rows"][0]["index"] = 0
    report["source_low"]["selection_summaries"]["count"] = 1
    document = parse_compile_report(report)
    assert build_barrier_show(document) is None
    assert "barriers" not in build_compile_report_show(document)


@pytest.mark.parametrize(
    ("mutation", "message"),
    [
        ("count", "count: does not match rows"),
        ("index", r"rows\[1\].index: expected 1"),
        ("dynamic", "dynamic evidence exceeds selected source operations"),
    ],
)
def test_show_rejects_inconsistent_barrier_summaries(
    mutation: str, message: str
) -> None:
    report = deepcopy(_report())
    summaries = report["source_low"]["selection_summaries"]
    if mutation == "count":
        summaries["count"] = 3
    elif mutation == "index":
        summaries["rows"][1]["index"] = 2
    else:
        summaries["rows"][0]["unknown_dynamic_op_count"] = 1
        summaries["rows"][0]["exact_dynamic_op_count"] = 1
    with pytest.raises(CompileReportError, match=message):
        build_barrier_show(parse_compile_report(report))
