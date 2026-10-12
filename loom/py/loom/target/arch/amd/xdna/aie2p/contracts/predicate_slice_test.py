# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Tests for AIE2P packed-predicate slice contracts."""

import pytest

from loom.dialect.vector import defs as vector
from loom.target.arch.amd.xdna.aie2p.contracts.predicate_concat_test import (
    _evaluate_descriptor,
    _pack_predicate_bits,
    _pattern_matches_lane_count,
    _unpack_predicate_bits,
    _value_type_guard,
)
from loom.target.arch.amd.xdna.aie2p.contracts.predicate_slice import (
    AIE2P_PREDICATE_SLICE_RULES,
)
from loom.target.arch.amd.xdna.aie2p.contracts.structural import (
    AIE2P_STRUCTURAL_RULES,
)
from loom.target.contracts import (
    AttrProject,
    AttrProjectKind,
    DescriptorRule,
    EmitDescriptorOp,
    EmitRegisterConcat,
    EmitRegisterSlice,
    GuardKind,
    TypePattern,
    ValueAliasRule,
    ValueRef,
)


def _offset_range(rule: DescriptorRule | ValueAliasRule) -> tuple[int, int]:
    guard = next(
        guard
        for guard in rule.guards
        if guard.kind is GuardKind.I64_ARRAY_ELEMENT_RANGE
        and guard.field == "static_offsets"
        and guard.element == 0
    )
    return guard.minimum, guard.maximum


def _predicate_slice_rules() -> tuple[DescriptorRule | ValueAliasRule, ...]:
    return tuple(
        rule
        for rule in AIE2P_STRUCTURAL_RULES
        if rule.source_op is vector.vector_slice
        and any(
            guard.kind is GuardKind.OPERAND_SEGMENT_COUNT
            and guard.field == "offsets"
            and guard.count == 0
            for guard in rule.guards
        )
        and _value_type_guard(rule, "source", 0).type_pattern.element == "i1"
    )


def _predicate_slice_rule_for_shape(
    source_lane_count: int,
    offset: int,
    result_lane_count: int,
) -> DescriptorRule | ValueAliasRule:
    matches = [
        rule
        for rule in _predicate_slice_rules()
        if _pattern_matches_lane_count(
            _value_type_guard(rule, "source", 0).type_pattern,
            source_lane_count,
        )
        and _pattern_matches_lane_count(
            _value_type_guard(rule, "result", 0).type_pattern,
            result_lane_count,
        )
        and _offset_range(rule)[0] <= offset <= _offset_range(rule)[1]
    ]
    assert matches, (source_lane_count, offset, result_lane_count)
    selected_priority = max(
        rule.priority if isinstance(rule, DescriptorRule) else 0 for rule in matches
    )
    matches = [
        rule
        for rule in matches
        if (rule.priority if isinstance(rule, DescriptorRule) else 0)
        == selected_priority
    ]
    assert len(matches) == 1, (
        source_lane_count,
        offset,
        result_lane_count,
        matches,
    )
    return matches[0]


def _evaluate_predicate_slice_rule(
    rule: DescriptorRule | ValueAliasRule,
    source_bits: tuple[int, ...],
    offset: int,
    result_lane_count: int,
) -> tuple[int, ...]:
    source = ValueRef.operand("source")
    packed_source = _pack_predicate_bits(source_bits)
    if isinstance(rule, ValueAliasRule):
        result = packed_source
    else:
        values: dict[ValueRef, int | tuple[int, ...]] = {source: packed_source}
        for emit in rule.emit:
            if isinstance(emit, EmitRegisterSlice):
                value = values[emit.source]
                assert isinstance(value, tuple)
                assert emit.unit_count in (None, 1)
                values[emit.result] = value[emit.unit_offset]
                continue
            if isinstance(emit, EmitRegisterConcat):
                carriers = tuple(values[value] for value in emit.sources)
                assert all(isinstance(carrier, int) for carrier in carriers)
                values[emit.result] = carriers
                continue

            assert isinstance(emit, EmitDescriptorOp)
            operands = {name: values[value] for name, value in emit.operands.items()}
            if emit.descriptor.key in (
                "amd.xdna.aie2p.constant.i32.short",
                "amd.xdna.aie2p.constant.i32.mova",
            ) and isinstance(emit.immediates["i"], AttrProject):
                immediate = emit.immediates["i"]
                assert immediate.kind is AttrProjectKind.I64_ARRAY_LITERAL_MINUS_ELEMENT
                assert immediate.source_attr == "static_offsets"
                assert immediate.element == 0
                descriptor_result = immediate.literal_i64 - offset
            else:
                descriptor_result = _evaluate_descriptor(emit, operands, 0, 0)
            assert len(emit.results) == 1
            values[next(iter(emit.results.values()))] = descriptor_result
        result = values[ValueRef.result("result")]

    assert isinstance(result, (int, tuple))
    return _unpack_predicate_bits(result, result_lane_count)


def _assert_predicate_slice(
    source_lane_count: int,
    offset: int,
    result_lane_count: int,
) -> None:
    rule = _predicate_slice_rule_for_shape(
        source_lane_count,
        offset,
        result_lane_count,
    )
    source_bits = tuple(
        int((lane * 5 + source_lane_count) % 7 < 3) for lane in range(source_lane_count)
    )
    assert (
        _evaluate_predicate_slice_rule(
            rule,
            source_bits,
            offset,
            result_lane_count,
        )
        == source_bits[offset : offset + result_lane_count]
    ), (
        source_lane_count,
        offset,
        result_lane_count,
    )


def _pattern_bounds(pattern: TypePattern) -> tuple[int, int]:
    if pattern.lanes is not None:
        return pattern.lanes, pattern.lanes
    assert isinstance(pattern.minimum_lanes, int)
    assert isinstance(pattern.maximum_lanes, int)
    return pattern.minimum_lanes, pattern.maximum_lanes


def test_predicate_slice_matches_every_partition_boundary() -> None:
    matched_rules = set()
    cases = set()
    for rule in _predicate_slice_rules():
        source_minimum, source_maximum = _pattern_bounds(
            _value_type_guard(rule, "source", 0).type_pattern
        )
        result_minimum, result_maximum = _pattern_bounds(
            _value_type_guard(rule, "result", 0).type_pattern
        )
        offset_minimum, offset_maximum = _offset_range(rule)
        for offset in {offset_minimum, offset_maximum}:
            for result_lane_count in {result_minimum, result_maximum}:
                required_source_lanes = offset + result_lane_count
                for source_lane_count in {
                    source_minimum,
                    source_maximum,
                    required_source_lanes,
                }:
                    if (
                        source_minimum <= source_lane_count <= source_maximum
                        and required_source_lanes <= source_lane_count
                    ):
                        cases.add((source_lane_count, offset, result_lane_count))

    for source_lane_count, offset, result_lane_count in sorted(cases):
        rule = _predicate_slice_rule_for_shape(
            source_lane_count,
            offset,
            result_lane_count,
        )
        matched_rules.add(id(rule))
        _assert_predicate_slice(source_lane_count, offset, result_lane_count)

    assert matched_rules == {id(rule) for rule in _predicate_slice_rules()}


def test_predicate_slice_uses_bounded_word_composition() -> None:
    assert len(AIE2P_PREDICATE_SLICE_RULES) == 34
    allowed_descriptors = {
        "amd.xdna.aie2p.constant.i32.short",
        "amd.xdna.aie2p.predicate.shift.low32",
        "amd.xdna.aie2p.predicate.shift.high32",
        "amd.xdna.aie2p.predicate.shift.low32.to.high32",
        "amd.xdna.aie2p.predicate.shift.high32.to.high32",
        "amd.xdna.aie2p.predicate.or.low32.rhs_tied",
        "amd.xdna.aie2p.predicate.or.low32.to.high32",
        "amd.xdna.aie2p.predicate.or.high32",
        "amd.xdna.aie2p.predicate.complete.zero.high32",
    }
    descriptor_counts = []
    emit_counts = []
    for rule in AIE2P_PREDICATE_SLICE_RULES:
        keys = [
            emit.descriptor.key
            for emit in rule.emit
            if isinstance(emit, EmitDescriptorOp)
        ]
        assert set(keys) <= allowed_descriptors
        descriptor_counts.append(len(keys))
        emit_counts.append(len(rule.emit))
    assert max(descriptor_counts) == 12, descriptor_counts
    assert max(emit_counts) == 15, emit_counts


def test_word_confined_slices_keep_their_minimal_path() -> None:
    for offset, result_lane_count, source_part, priority in (
        (2, 2, "low32", 1),
        (8, 8, "low32", 1),
        (16, 16, "low32", 1),
        (24, 8, "low32", 1),
        (32, 32, "high32", 0),
        (48, 16, "high32", 0),
    ):
        rule = _predicate_slice_rule_for_shape(64, offset, result_lane_count)
        assert isinstance(rule, DescriptorRule)
        assert rule.priority == priority
        assert [emit.descriptor.key for emit in rule.emit] == [
            "amd.xdna.aie2p.constant.i32.short",
            f"amd.xdna.aie2p.predicate.shift.{source_part}",
            "amd.xdna.aie2p.predicate.complete.zero.high32",
        ]

    wide_rule = _predicate_slice_rule_for_shape(128, 66, 2)
    assert isinstance(wide_rule, DescriptorRule)
    assert wide_rule.priority == 1
    assert isinstance(wide_rule.emit[0], EmitRegisterSlice)
    assert wide_rule.emit[0].unit_offset == 1
    assert [
        emit.descriptor.key
        for emit in wide_rule.emit
        if isinstance(emit, EmitDescriptorOp)
    ] == [
        "amd.xdna.aie2p.constant.i32.short",
        "amd.xdna.aie2p.predicate.shift.low32",
        "amd.xdna.aie2p.predicate.complete.zero.high32",
    ]


@pytest.mark.exhaustive
def test_predicate_slice_covers_every_static_interval() -> None:
    for source_lane_count in range(1, 129):
        for offset in range(source_lane_count):
            for result_lane_count in range(1, source_lane_count - offset + 1):
                _assert_predicate_slice(
                    source_lane_count,
                    offset,
                    result_lane_count,
                )
