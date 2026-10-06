# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Wait-reason views, semantic diffs, and text formatting."""

from __future__ import annotations

from dataclasses import dataclass

from loom.reporting.compile_report import CompileReportError

_COUNT_FIELDS = (
    "action_count",
    "explicit_action_count",
    "planned_action_count",
    "full_drain_count",
    "partial_wait_count",
    "drained_count",
    "max_drained_count",
    "max_outstanding_before",
    "max_full_drain_outstanding_before",
)

_COUNT_LABELS = {
    "action_count": "actions",
    "explicit_action_count": "explicit actions",
    "planned_action_count": "planned actions",
    "full_drain_count": "full drains",
    "partial_wait_count": "partial waits",
    "drained_count": "packets drained",
    "max_drained_count": "maximum packets drained",
    "max_outstanding_before": "maximum block-local outstanding",
    "max_full_drain_outstanding_before": (
        "maximum block-local outstanding before a full drain"
    ),
}


@dataclass(frozen=True)
class CompileReportWaitReason:
    """One target-classified reason for logical wait actions."""

    function: str
    counter: str
    counter_id: int
    reason: str
    reason_id: int
    counts: tuple[int, ...]

    def to_json_object(self) -> dict[str, object]:
        """Returns stable public fields for this reason row."""
        return {
            "counter": self.counter,
            "reason": self.reason,
            **dict(zip(_COUNT_FIELDS, self.counts, strict=True)),
        }


@dataclass(frozen=True)
class CompileReportWaitReasonInventory:
    """Validated wait reasons indexed once by target function."""

    by_function: dict[str, tuple[CompileReportWaitReason, ...]]


def parse_compile_report_wait_reasons(
    report: dict[str, object],
    entries: tuple[dict[str, object], ...],
    document_source: str,
) -> CompileReportWaitReasonInventory | None:
    """Parses and validates optional wait reasons once for one report."""
    value = report.get("wait_reason_summary_rows")
    if value is None:
        return None
    source = f"{document_source}.wait_reason_summary_rows"
    collection = _require_object(value, source)
    count = _require_count(collection.get("count"), f"{source}.count")
    rows_value = collection.get("rows")
    if rows_value is None:
        if count != 0:
            raise CompileReportError(f"{source}.rows: missing {count} rows")
        rows = []
    else:
        rows = _require_list(rows_value, f"{source}.rows")
        if count != len(rows):
            raise CompileReportError(
                f"{source}.count: expected {len(rows)}, got {count}"
            )

    counter_names_by_id: dict[int, str] = {}
    counter_ids_by_name: dict[str, int] = {}
    reason_names_by_id: dict[int, str] = {}
    reason_ids_by_name: dict[str, int] = {}
    seen_identities: set[tuple[str, int, int]] = set()
    by_function: dict[str, list[CompileReportWaitReason]] = {}
    for position, row_value in enumerate(rows):
        row_source = f"{source}.rows[{position}]"
        row = _require_object(row_value, row_source)
        index = _require_count(row.get("index"), f"{row_source}.index")
        if index != position:
            raise CompileReportError(
                f"{row_source}.index: expected {position}, got {index}"
            )
        function = _require_string(row.get("function"), f"{row_source}.function")
        counter = _require_string(row.get("counter"), f"{row_source}.counter")
        counter_id = _require_count(row.get("counter_id"), f"{row_source}.counter_id")
        reason = _require_string(row.get("reason"), f"{row_source}.reason")
        reason_id = _require_count(row.get("reason_id"), f"{row_source}.reason_id")
        _record_stable_identity(
            counter,
            counter_id,
            counter_names_by_id,
            counter_ids_by_name,
            f"{row_source}.counter",
        )
        _record_stable_identity(
            reason,
            reason_id,
            reason_names_by_id,
            reason_ids_by_name,
            f"{row_source}.reason",
        )
        identity = (function, counter_id, reason_id)
        if identity in seen_identities:
            raise CompileReportError(
                f"{row_source}: duplicate wait reason {counter!r}/{reason!r} "
                f"for function {function!r}"
            )
        seen_identities.add(identity)
        counts = _parse_counts(row.get("summary"), f"{row_source}.summary")
        by_function.setdefault(function, []).append(
            CompileReportWaitReason(
                function=function,
                counter=counter,
                counter_id=counter_id,
                reason=reason,
                reason_id=reason_id,
                counts=counts,
            )
        )

    entries_by_function: dict[str, dict[str, object]] = {}
    for entry in entries:
        function_value = entry.get("function")
        if not isinstance(function_value, str) or not function_value:
            continue
        if function_value in entries_by_function:
            if function_value in by_function:
                raise CompileReportError(
                    f"{source}: wait reasons cannot select duplicate function "
                    f"{function_value!r}"
                )
            continue
        entries_by_function[function_value] = entry
    for function in by_function:
        entry = entries_by_function.get(function)
        if entry is None:
            raise CompileReportError(
                f"{source}: wait reasons reference unknown function {function!r}"
            )

    return CompileReportWaitReasonInventory(
        by_function={
            function: tuple(
                sorted(function_rows, key=lambda row: (row.counter, row.reason))
            )
            for function, function_rows in by_function.items()
        }
    )


def build_wait_reason_show(
    inventory: CompileReportWaitReasonInventory | None,
    function: str | None,
) -> dict[str, object] | None:
    """Builds one entry's compact wait-reason view."""
    if inventory is None or function is None:
        return None
    reasons = inventory.by_function.get(function, ())
    if not reasons:
        return None
    return {
        "reason_count": len(reasons),
        "reasons": [reason.to_json_object() for reason in reasons],
    }


def build_wait_reason_diff(
    baseline: CompileReportWaitReasonInventory | None,
    candidate: CompileReportWaitReasonInventory | None,
    baseline_function: str | None,
    candidate_function: str | None,
) -> dict[str, object] | None:
    """Builds one paired entry's semantic wait-reason diff."""
    if baseline is None and candidate is None:
        return None
    if baseline is None or candidate is None:
        return {
            "availability": {
                "baseline": "available" if baseline is not None else "unavailable",
                "candidate": ("available" if candidate is not None else "unavailable"),
            }
        }

    baseline_by_reason = _reasons_by_identity(baseline, baseline_function)
    candidate_by_reason = _reasons_by_identity(candidate, candidate_function)
    changed_reasons = []
    unchanged_reason_count = 0
    for identity in sorted(set(baseline_by_reason) | set(candidate_by_reason)):
        baseline_reason = baseline_by_reason.get(identity)
        candidate_reason = candidate_by_reason.get(identity)
        if baseline_reason is None:
            changed_reasons.append(
                {
                    "counter": identity[0],
                    "reason": identity[1],
                    "status": "added",
                    "candidate": candidate_reason.to_json_object(),
                }
            )
            continue
        if candidate_reason is None:
            changed_reasons.append(
                {
                    "counter": identity[0],
                    "reason": identity[1],
                    "status": "removed",
                    "baseline": baseline_reason.to_json_object(),
                }
            )
            continue
        changes = {
            field: _count_change(baseline_count, candidate_count)
            for field, baseline_count, candidate_count in zip(
                _COUNT_FIELDS,
                baseline_reason.counts,
                candidate_reason.counts,
                strict=True,
            )
            if baseline_count != candidate_count
        }
        if not changes:
            unchanged_reason_count += 1
            continue
        changed_reasons.append(
            {
                "counter": identity[0],
                "reason": identity[1],
                "status": "changed",
                "changes": changes,
            }
        )
    return {
        "changed_reason_count": len(changed_reasons),
        "unchanged_reason_count": unchanged_reason_count,
        "reasons": changed_reasons,
    }


def wait_reason_diff_has_changes(diff: dict[str, object] | None) -> bool:
    """Returns whether a wait-reason diff contains changed evidence."""
    if diff is None:
        return False
    availability = diff.get("availability")
    if isinstance(availability, dict):
        return availability["baseline"] != availability["candidate"]
    return bool(diff.get("changed_reason_count", 0))


def append_wait_reason_show_text(
    lines: list[str], wait_reasons: dict[str, object]
) -> None:
    """Appends human-readable wait reasons for one entry."""
    lines.append("  Wait reasons (compiler analysis)")
    lines.append(f"    counter/reason groups: {wait_reasons['reason_count']}")
    for reason_value in _require_list(wait_reasons["reasons"], "reasons"):
        reason = _require_object(reason_value, "reason")
        lines.append(f"    {reason['counter']} / {reason['reason']}:")
        lines.append(f"      {_format_reason_counts(reason)}")


def append_wait_reason_diff_text(
    lines: list[str], wait_reasons: dict[str, object]
) -> None:
    """Appends a human-readable wait-reason diff for one entry."""
    lines.append("  Wait reasons (compiler analysis)")
    availability = wait_reasons.get("availability")
    if isinstance(availability, dict):
        lines.append(
            f"    evidence: {availability['baseline']} -> {availability['candidate']}"
        )
        return
    lines.append(
        f"    groups: {wait_reasons['changed_reason_count']} changed, "
        f"{wait_reasons['unchanged_reason_count']} unchanged"
    )
    for reason_value in _require_list(wait_reasons["reasons"], "reasons"):
        reason = _require_object(reason_value, "reason")
        label = f"{reason['counter']} / {reason['reason']}"
        status = reason["status"]
        if status == "added":
            candidate = _require_object(reason["candidate"], "reason.candidate")
            lines.append(f"    {label}: added")
            lines.append(f"      {_format_reason_counts(candidate)}")
        elif status == "removed":
            baseline = _require_object(reason["baseline"], "reason.baseline")
            lines.append(f"    {label}: removed")
            lines.append(f"      {_format_reason_counts(baseline)}")
        else:
            lines.append(f"    {label}: changed")
            changes = _require_object(reason["changes"], "reason.changes")
            for field in _COUNT_FIELDS:
                change_value = changes.get(field)
                if change_value is None:
                    continue
                lines.append(
                    f"      {_format_field_name(field)}: "
                    f"{_format_count_change(_require_object(change_value, field))}"
                )


def _parse_counts(value: object, source: str) -> tuple[int, ...]:
    summary = _require_object(value, source)
    counts = tuple(
        _require_count(summary.get(field), f"{source}.{field}")
        for field in _COUNT_FIELDS
    )
    values = dict(zip(_COUNT_FIELDS, counts, strict=True))
    if values["action_count"] == 0:
        raise CompileReportError(f"{source}.action_count: expected a positive count")
    if values["action_count"] != (
        values["explicit_action_count"] + values["planned_action_count"]
    ):
        raise CompileReportError(
            f"{source}: action count must equal explicit plus planned actions"
        )
    if values["action_count"] != (
        values["full_drain_count"] + values["partial_wait_count"]
    ):
        raise CompileReportError(
            f"{source}: action count must equal full drains plus partial waits"
        )
    if values["max_drained_count"] > values["drained_count"]:
        raise CompileReportError(
            f"{source}.max_drained_count: exceeds total drained count"
        )
    if values["max_drained_count"] > values["max_outstanding_before"]:
        raise CompileReportError(
            f"{source}.max_drained_count: exceeds maximum outstanding count"
        )
    if values["max_full_drain_outstanding_before"] > values["max_outstanding_before"]:
        raise CompileReportError(
            f"{source}.max_full_drain_outstanding_before: exceeds maximum "
            "outstanding count"
        )
    if (
        values["full_drain_count"] == 0
        and values["max_full_drain_outstanding_before"] != 0
    ):
        raise CompileReportError(
            f"{source}.max_full_drain_outstanding_before: requires a full drain"
        )
    return counts


def _record_stable_identity(
    name: str,
    identity: int,
    names_by_id: dict[int, str],
    ids_by_name: dict[str, int],
    source: str,
) -> None:
    previous_name = names_by_id.setdefault(identity, name)
    previous_id = ids_by_name.setdefault(name, identity)
    if previous_name != name or previous_id != identity:
        raise CompileReportError(f"{source}: inconsistent stable name/id mapping")


def _reasons_by_identity(
    inventory: CompileReportWaitReasonInventory,
    function: str | None,
) -> dict[tuple[str, str], CompileReportWaitReason]:
    if function is None:
        return {}
    return {
        (reason.counter, reason.reason): reason
        for reason in inventory.by_function.get(function, ())
    }


def _count_change(baseline: int, candidate: int) -> dict[str, object]:
    change: dict[str, object] = {
        "baseline": baseline,
        "candidate": candidate,
        "delta": candidate - baseline,
    }
    if baseline != 0:
        change["change_percent"] = (candidate - baseline) * 100.0 / baseline
    return change


def _format_reason_counts(reason: dict[str, object]) -> str:
    return (
        f"{_format_counted(reason['action_count'], 'action')} "
        f"({reason['explicit_action_count']} explicit, "
        f"{reason['planned_action_count']} planned); "
        f"{_format_counted(reason['full_drain_count'], 'full drain')}, "
        f"{_format_counted(reason['partial_wait_count'], 'partial wait')}; "
        f"{_format_counted(reason['drained_count'], 'packet')} drained; maxima "
        f"{reason['max_drained_count']} drained, "
        f"{reason['max_outstanding_before']} block-local outstanding, "
        f"{reason['max_full_drain_outstanding_before']} before a full drain"
    )


def _format_count_change(change: dict[str, object]) -> str:
    return (
        f"{change['baseline']} -> {change['candidate']}, "
        f"delta {int(change['delta']):+d}"
    )


def _format_field_name(field: str) -> str:
    return _COUNT_LABELS[field]


def _format_counted(value: object, singular: str) -> str:
    count = int(value)
    suffix = "" if count == 1 else "s"
    return f"{count} {singular}{suffix}"


def _require_object(value: object, source: str) -> dict[str, object]:
    if not isinstance(value, dict):
        raise CompileReportError(f"{source}: expected object")
    return value


def _require_list(value: object, source: str) -> list[object]:
    if not isinstance(value, list):
        raise CompileReportError(f"{source}: expected array")
    return value


def _require_string(value: object, source: str) -> str:
    if not isinstance(value, str) or not value:
        raise CompileReportError(f"{source}: expected non-empty string")
    return value


def _require_count(value: object, source: str) -> int:
    if not isinstance(value, int) or isinstance(value, bool) or value < 0:
        raise CompileReportError(f"{source}: expected non-negative integer")
    return value
