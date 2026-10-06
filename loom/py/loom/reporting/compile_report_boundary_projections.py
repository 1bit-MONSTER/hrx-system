# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Source boundary representations and actionable projection experiments."""

from __future__ import annotations

from typing import cast

from loom.reporting.compile_report import CompileReportDocument, CompileReportError
from loom.reporting.compile_report_suggestions import (
    CompileReportSuggestion,
    CompileReportSuggestionEvidence,
)

_PATH = "source_low.boundary_projections"
_OUTCOMES = ("selected", "preserved", "rejected")

_ACTIONABLE_REJECTIONS = {
    "non_static_component_access": (
        "vector.compare_static_bank_access",
        "uses a dynamic component selection. If the component domain is small "
        "and bounded, compare a formulation that statically unrolls that "
        "selection or carries the components explicitly",
    ),
    "inconsistent_component_access": (
        "vector.compare_uniform_bank_components",
        "mixes component shapes or prefix ranks. Compare a formulation where "
        "every access selects the same component shape",
    ),
    "unsupported_value_use": (
        "vector.compare_componentwise_bank_state",
        "has static component accesses plus a consumer that still requires the "
        "whole bank. Compare a formulation that expresses that consumer with "
        "the same componentwise state",
    ),
}


def build_boundary_projection_show(
    document: CompileReportDocument,
) -> dict[str, object] | None:
    """Validates and returns source boundary-representation decisions."""
    source_low = document.report.get("source_low")
    if source_low is None:
        return None
    source_low_object = _object(source_low, f"{document.source}.source_low")
    value = source_low_object.get("boundary_projections")
    if value is None:
        return None
    path = f"{document.source}.{_PATH}"
    section = _object(value, path)
    counts = {
        "count": _count(section.get("count"), f"{path}.count"),
        "selected_count": _count(
            section.get("selected_count"), f"{path}.selected_count"
        ),
        "preserved_count": _count(
            section.get("preserved_count"), f"{path}.preserved_count"
        ),
        "rejected_count": _count(
            section.get("rejected_count"), f"{path}.rejected_count"
        ),
    }
    if counts["count"] != sum(counts[f"{outcome}_count"] for outcome in _OUTCOMES):
        raise CompileReportError(f"{path}: outcome counts do not sum to count")

    rows_value = section.get("rows")
    if document.mode == "summary":
        if rows_value is not None:
            raise CompileReportError(f"{path}.rows: unexpected in summary report")
        return counts
    if rows_value is None:
        raise CompileReportError(f"{path}.rows: detailed report is missing rows")
    rows = _array(rows_value, f"{path}.rows")
    if len(rows) != counts["count"]:
        raise CompileReportError(f"{path}.count: does not match decision rows")

    decisions = []
    identities: set[tuple[object, ...]] = set()
    actual_counts = {outcome: 0 for outcome in _OUTCOMES}
    for position, value in enumerate(rows):
        row_path = f"{path}.rows[{position}]"
        row = _object(value, row_path)
        index = _count(row.get("index"), f"{row_path}.index")
        if index != position:
            raise CompileReportError(f"{row_path}.index: expected {position}")
        function = _string(row.get("function"), f"{row_path}.function")
        source_op = _string(row.get("source_op"), f"{row_path}.source_op")
        source_op_kind = _count(row.get("source_op_kind"), f"{row_path}.source_op_kind")
        projection = _string(row.get("projection"), f"{row_path}.projection")
        boundary = _string(row.get("boundary"), f"{row_path}.boundary")
        outcome = _string(row.get("outcome"), f"{row_path}.outcome")
        if outcome not in _OUTCOMES:
            raise CompileReportError(f"{row_path}.outcome: unsupported outcome")
        reason = _string(row.get("reason"), f"{row_path}.reason")
        operation = _count(row.get("operation"), f"{row_path}.operation")
        source_value = _count(row.get("source_value"), f"{row_path}.source_value")
        identity = (function, operation, source_value, projection, boundary)
        if identity in identities:
            raise CompileReportError(f"{row_path}: duplicate boundary identity")
        identities.add(identity)

        source_type_kind = _count(
            row.get("source_type_kind"), f"{row_path}.source_type_kind"
        )
        source_type = _string(row.get("source_type"), f"{row_path}.source_type")
        source_element_type = _count(
            row.get("source_element_type"), f"{row_path}.source_element_type"
        )
        source_element = _string(
            row.get("source_element"), f"{row_path}.source_element"
        )
        source_rank = _count(row.get("source_rank"), f"{row_path}.source_rank")
        source_shape = _shape(
            row.get("source_shape"), f"{row_path}.source_shape", source_rank
        )
        prefix_rank = _count(
            row.get("projected_prefix_rank"),
            f"{row_path}.projected_prefix_rank",
        )
        if prefix_rank > source_rank:
            raise CompileReportError(
                f"{row_path}.projected_prefix_rank: exceeds source rank"
            )
        component_count = _count(
            row.get("component_count"), f"{row_path}.component_count"
        )
        component_shape_value = row.get("component_shape")
        component_shape: tuple[int, ...] | None = None
        if component_count:
            if component_shape_value is None:
                raise CompileReportError(
                    f"{row_path}.component_shape: missing projected shape"
                )
            component_shape = _shape(
                component_shape_value,
                f"{row_path}.component_shape",
                source_rank - prefix_rank,
            )
            if component_shape != source_shape[prefix_rank:]:
                raise CompileReportError(
                    f"{row_path}.component_shape: does not match source suffix"
                )
            prefix_shape = source_shape[:prefix_rank]
            if all(dimension > 0 for dimension in prefix_shape):
                expected_count = 1
                for dimension in prefix_shape:
                    expected_count *= dimension
                if component_count != expected_count:
                    raise CompileReportError(
                        f"{row_path}.component_count: expected {expected_count}"
                    )
        elif component_shape_value is not None:
            raise CompileReportError(
                f"{row_path}.component_shape: present without components"
            )
        if outcome == "selected" and component_count == 0:
            raise CompileReportError(
                f"{row_path}.component_count: selected projection has no components"
            )

        decision: dict[str, object] = {
            "index": index,
            "function": function,
            "source_op": source_op,
            "source_op_kind": source_op_kind,
            "projection": projection,
            "boundary": boundary,
            "outcome": outcome,
            "reason": reason,
            "operation": operation,
            "source_value": source_value,
            "source_type_kind": source_type_kind,
            "source_type": source_type,
            "source_element_type": source_element_type,
            "source_element": source_element,
            "source_rank": source_rank,
            "source_shape": list(source_shape),
            "source_type_text": _type_text(source_type, source_element, source_shape),
            "projected_prefix_rank": prefix_rank,
            "component_count": component_count,
        }
        if component_shape is not None:
            decision["component_shape"] = list(component_shape)
            decision["component_type_text"] = _type_text(
                source_type, source_element, component_shape
            )
        decisions.append(decision)
        actual_counts[outcome] += 1

    for outcome in _OUTCOMES:
        if actual_counts[outcome] != counts[f"{outcome}_count"]:
            raise CompileReportError(
                f"{path}.{outcome}_count: does not match decision rows"
            )
    return {**counts, "rows": decisions}


def append_boundary_projection_show_text(
    lines: list[str], view: dict[str, object]
) -> None:
    """Appends source boundary representation decisions."""
    lines.extend(
        (
            "",
            "Source boundary projections",
            (
                f"  selected={view['selected_count']} "
                f"preserved={view['preserved_count']} "
                f"rejected={view['rejected_count']}"
            ),
        )
    )
    for row in cast(list[dict[str, object]], view.get("rows", [])):
        line = (
            f"  {row['function']} {row['source_op']}[{row['operation']}] "
            f"{row['boundary']}[{row['source_value']}]: {row['projection']} "
            f"{row['outcome']} {row['source_type_text']}"
        )
        if row["component_count"]:
            line += f" -> {row['component_count']} x {row['component_type_text']}"
        lines.append(f"{line} reason={row['reason']}")


def suggest_boundary_projections(
    document: CompileReportDocument,
) -> tuple[CompileReportSuggestion, ...]:
    """Proposes source experiments only for actionable rejected projections."""
    view = build_boundary_projection_show(document)
    if view is None or document.status_code != 0 or "rows" not in view:
        return ()
    suggestions = []
    for position, row in enumerate(cast(list[dict[str, object]], view["rows"])):
        if row["outcome"] != "rejected":
            continue
        action = _ACTIONABLE_REJECTIONS.get(cast(str, row["reason"]))
        if action is None:
            continue
        suggestion_id, experiment = action
        path = f"{_PATH}.rows[{position}]"
        evidence = tuple(
            CompileReportSuggestionEvidence(f"{path}.{key}", row[key])
            for key in (
                "function",
                "source_op",
                "operation",
                "source_value",
                "projection",
                "boundary",
                "outcome",
                "reason",
                "source_shape",
                "projected_prefix_rank",
                "component_count",
            )
        )
        suggestions.append(
            CompileReportSuggestion(
                suggestion_id=suggestion_id,
                entry_name=cast(str, row["function"]),
                action=(
                    f"{row['source_op']}[{row['operation']}] state "
                    f"{row['source_value']} in {row['function']} {experiment}. "
                    "Check that the boundary projection becomes selected, then "
                    "compare final registers, spills, occupancy, code size, "
                    "compile time, and measured runtime with the workload and "
                    "schedule held fixed. Selection enables an experiment; it "
                    "does not by itself establish a performance gain."
                ),
                evidence=evidence,
            )
        )
    return tuple(suggestions)


def _type_text(kind: str, element: str, shape: tuple[int, ...]) -> str:
    if not shape:
        return element if kind == "vector" else kind
    dimensions = "".join(
        f"{'?' if dimension == -1 else dimension}x" for dimension in shape
    )
    return f"{kind}<{dimensions}{element}>"


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
        raise CompileReportError(f"{path}: expected nonempty string")
    return value


def _count(value: object, path: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value < 0:
        raise CompileReportError(f"{path}: expected nonnegative integer")
    return value


def _shape(value: object, path: str, rank: int) -> tuple[int, ...]:
    dimensions = _array(value, path)
    if len(dimensions) != rank:
        raise CompileReportError(f"{path}: expected {rank} dimensions")
    result = []
    for axis, dimension in enumerate(dimensions):
        if (
            isinstance(dimension, bool)
            or not isinstance(dimension, int)
            or (dimension < 1 and dimension != -1)
        ):
            raise CompileReportError(f"{path}[{axis}]: expected positive extent or -1")
        result.append(dimension)
    return tuple(result)
