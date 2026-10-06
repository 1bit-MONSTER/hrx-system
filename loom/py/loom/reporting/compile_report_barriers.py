# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 WITH LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Authored barrier phases and their selected target realization."""

from __future__ import annotations

from loom.reporting.compile_report import CompileReportDocument, CompileReportError

_BARRIER_PHASES = {
    "kernel.barrier": "complete",
    "kernel.barrier.arrive": "arrive",
    "kernel.barrier.wait": "wait",
}


def build_barrier_show(
    document: CompileReportDocument,
) -> dict[str, object] | None:
    """Builds a bounded view of source barrier lowering summaries."""
    source_low_value = document.report.get("source_low")
    if source_low_value is None:
        return None
    source_low = _object(source_low_value, f"{document.source}.source_low")
    summaries_value = source_low.get("selection_summaries")
    if summaries_value is None:
        return None
    path = f"{document.source}.source_low.selection_summaries"
    summaries = _object(summaries_value, path)
    rows = _array(summaries.get("rows"), f"{path}.rows")
    if _count(summaries.get("count"), f"{path}.count") != len(rows):
        raise CompileReportError(f"{path}.count: does not match summary rows")

    barrier_rows = []
    for position, value in enumerate(rows):
        row_path = f"{path}.rows[{position}]"
        row = _object(value, row_path)
        if _count(row.get("index"), f"{row_path}.index") != position:
            raise CompileReportError(f"{row_path}.index: expected {position}")
        operation = _string(row.get("source_op"), f"{row_path}.source_op")
        phase = _BARRIER_PHASES.get(operation)
        if phase is None:
            continue

        selected_count = _count(
            row.get("selected_op_count"), f"{row_path}.selected_op_count"
        )
        if selected_count == 0:
            raise CompileReportError(
                f"{row_path}.selected_op_count: expected positive count"
            )
        exact_count = _optional_count(
            row.get("exact_dynamic_op_count"),
            f"{row_path}.exact_dynamic_op_count",
        )
        unknown_count = _optional_count(
            row.get("unknown_dynamic_op_count"),
            f"{row_path}.unknown_dynamic_op_count",
        )
        if exact_count + unknown_count > selected_count:
            raise CompileReportError(
                f"{row_path}: dynamic evidence exceeds selected source operations"
            )

        barrier_row: dict[str, object] = {
            "function": _string(row.get("function"), f"{row_path}.function"),
            "phase": phase,
            "source_op": operation,
            "selection": _string(row.get("selection"), f"{row_path}.selection"),
            "selected_op_count": selected_count,
            "emitted_low_op_count": _count(
                row.get("emitted_low_op_count"),
                f"{row_path}.emitted_low_op_count",
            ),
        }
        for source_key, view_key in (
            ("plan_key", "plan_key"),
            ("descriptor_key", "descriptor_key"),
            ("descriptor_semantic_tag", "descriptor_semantic_tag"),
        ):
            field = _optional_string(row.get(source_key), f"{row_path}.{source_key}")
            if field is not None:
                barrier_row[view_key] = field
        if exact_count or unknown_count:
            barrier_row["dynamic"] = {
                "exact_source_op_count": exact_count,
                "unknown_source_op_count": unknown_count,
                "selected_op_count": _optional_count(
                    row.get("dynamic_selected_op_count"),
                    f"{row_path}.dynamic_selected_op_count",
                ),
                "emitted_low_op_count": _optional_count(
                    row.get("dynamic_emitted_low_op_count"),
                    f"{row_path}.dynamic_emitted_low_op_count",
                ),
            }
        barrier_rows.append(barrier_row)

    if not barrier_rows:
        return None
    return {"count": len(barrier_rows), "rows": barrier_rows}


def append_barrier_show_text(lines: list[str], barriers: dict[str, object]) -> None:
    """Appends the human-readable authored barrier view."""
    lines.extend(("", "Barrier realization (compiler analysis)"))
    for value in _array(barriers["rows"], "barrier rows"):
        row = _object(value, "barrier row")
        realization = f"selection={row['selection']}"
        for key, label in (
            ("plan_key", "plan"),
            ("descriptor_key", "descriptor"),
            ("descriptor_semantic_tag", "semantic"),
        ):
            if key in row:
                realization += f" {label}={row[key]}"
        static = (
            f"static={row['selected_op_count']} source/"
            f"{row['emitted_low_op_count']} low"
        )
        dynamic_value = row.get("dynamic")
        dynamic = ""
        if isinstance(dynamic_value, dict):
            exact_count = dynamic_value["exact_source_op_count"]
            unknown_count = dynamic_value["unknown_source_op_count"]
            if exact_count:
                dynamic = (
                    f" dynamic={dynamic_value['selected_op_count']} source/"
                    f"{dynamic_value['emitted_low_op_count']} low"
                )
                if unknown_count:
                    dynamic += f" + {unknown_count} source op unknown"
            else:
                dynamic = f" dynamic=unknown ({unknown_count} source op)"
        lines.append(
            f"  {row['function']} {row['phase']} ({row['source_op']}): "
            f"{realization} {static}{dynamic}"
        )


def _object(value: object, path: str) -> dict[str, object]:
    if not isinstance(value, dict):
        raise CompileReportError(f"{path}: expected object")
    return value


def _array(value: object, path: str) -> list[object]:
    if not isinstance(value, list):
        raise CompileReportError(f"{path}: expected array")
    return value


def _string(value: object, path: str) -> str:
    if not isinstance(value, str) or not value:
        raise CompileReportError(f"{path}: expected non-empty string")
    return value


def _optional_string(value: object, path: str) -> str | None:
    if value is None:
        return None
    return _string(value, path)


def _count(value: object, path: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value < 0:
        raise CompileReportError(f"{path}: expected non-negative integer")
    return value


def _optional_count(value: object, path: str) -> int:
    return 0 if value is None else _count(value, path)
