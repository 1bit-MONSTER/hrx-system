# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Tests for AIE2P packed-predicate concat contracts."""

import pytest

from loom.target.arch.amd.xdna.aie2p.contracts.predicate_concat import (
    AIE2P_PREDICATE_CONCAT_RULES,
)
from loom.target.contracts import (
    DescriptorRule,
    EmitDescriptorOp,
    EmitRegisterConcat,
    EmitRegisterSlice,
    GuardKind,
    ValueRef,
    ValueTypeProject,
    ValueTypeProjectKind,
)

_LOW_WORD_MASK = (1 << 32) - 1
_CARRIER_MASK = (1 << 64) - 1


def _pattern_matches_lane_count(pattern, lane_count: int) -> bool:
    if pattern.lanes is not None:
        return pattern.lanes == lane_count
    assert isinstance(pattern.minimum_lanes, int)
    assert isinstance(pattern.maximum_lanes, int)
    return pattern.minimum_lanes <= lane_count <= pattern.maximum_lanes


def _value_type_guard(
    rule: DescriptorRule,
    field: str,
    element: int | None,
):
    return next(
        guard
        for guard in rule.guards
        if guard.kind is GuardKind.VALUE_TYPE
        and guard.field == field
        and guard.element == element
    )


def _predicate_concat_rule_for_lanes(
    left_lane_count: int,
    right_lane_count: int,
) -> DescriptorRule:
    result_lane_count = left_lane_count + right_lane_count
    matches = [
        rule
        for rule in AIE2P_PREDICATE_CONCAT_RULES
        if _pattern_matches_lane_count(
            _value_type_guard(rule, "inputs", 0).type_pattern,
            left_lane_count,
        )
        and _pattern_matches_lane_count(
            _value_type_guard(rule, "inputs", 1).type_pattern,
            right_lane_count,
        )
        and _pattern_matches_lane_count(
            _value_type_guard(rule, "result", 0).type_pattern,
            result_lane_count,
        )
    ]
    assert len(matches) == 1, (left_lane_count, right_lane_count, matches)
    return matches[0]


def _pack_predicate_bits(bits: tuple[int, ...]) -> int | tuple[int, ...]:
    """Packs active bits and poisons every inactive carrier bit with one."""

    carriers = []
    for offset in range(0, len(bits), 64):
        carrier = _CARRIER_MASK
        for lane, bit in enumerate(bits[offset : offset + 64]):
            if not bit:
                carrier &= ~(1 << lane)
        carriers.append(carrier)
    return carriers[0] if len(carriers) == 1 else tuple(carriers)


def _unpack_predicate_bits(
    value: int | tuple[int, ...],
    lane_count: int,
) -> tuple[int, ...]:
    carriers = (value,) if isinstance(value, int) else value
    return tuple(
        (carriers[lane // 64] >> (lane % 64)) & 1 for lane in range(lane_count)
    )


def _evaluate_type_project(
    project: ValueTypeProject,
    left_lane_count: int,
    right_lane_count: int,
) -> int:
    lane_counts = {
        ValueRef.operand("inputs", element=0): left_lane_count,
        ValueRef.operand("inputs", element=1): right_lane_count,
        ValueRef.result("result"): left_lane_count + right_lane_count,
    }
    projected = lane_counts[project.source] * project.scale
    if project.kind is ValueTypeProjectKind.STATIC_DIM_SCALED:
        return projected + project.literal_i64
    if project.kind is ValueTypeProjectKind.LITERAL_MINUS_STATIC_DIM_SCALED:
        return project.literal_i64 - projected
    assert project.kind is ValueTypeProjectKind.STATIC_DIM_LOW_BITS_MASK
    width = projected + project.literal_i64
    assert 0 <= width <= 32
    return (1 << width) - 1 if width < 32 else _LOW_WORD_MASK


def _predicate_word(carrier: int, part: str) -> int:
    if part == "low32":
        return carrier & _LOW_WORD_MASK
    assert part == "high32"
    return (carrier >> 32) & _LOW_WORD_MASK


def _shift_word(word: int, shift: int) -> int:
    shifted = word << shift if shift >= 0 else word >> -shift
    return shifted & _LOW_WORD_MASK


def _evaluate_descriptor(
    emit: EmitDescriptorOp,
    operands: dict[str, int | tuple[int, ...]],
    left_lane_count: int,
    right_lane_count: int,
) -> int:
    key = emit.descriptor.key
    if key in (
        "amd.xdna.aie2p.constant.i32.short",
        "amd.xdna.aie2p.constant.i32.mova",
    ):
        immediate = emit.immediates["i"]
        return (
            _evaluate_type_project(
                immediate,
                left_lane_count,
                right_lane_count,
            )
            if isinstance(immediate, ValueTypeProject)
            else immediate
        )

    if ".predicate.mask." in key:
        source_part = "high32" if ".mask.high32." in key else "low32"
        source = operands["s0"]
        mask = operands["s1"]
        assert isinstance(source, int)
        assert isinstance(mask, int)
        return _predicate_word(source, source_part) & mask

    if ".predicate.shift." in key:
        source_part = "high32" if ".shift.high32" in key else "low32"
        destination_part = "high32" if key.endswith(".to.high32") else "low32"
        source = operands["s0"]
        shift = operands["s1"]
        assert isinstance(source, int)
        assert isinstance(shift, int)
        shifted = _shift_word(_predicate_word(source, source_part), shift)
        if destination_part == "low32":
            return shifted
        storage = operands["storage"]
        assert isinstance(storage, int)
        return (storage & _LOW_WORD_MASK) | (shifted << 32)

    if key == "amd.xdna.aie2p.predicate.or.low32.rhs_tied":
        lhs = operands["s0"]
        rhs = operands["s1"]
        assert isinstance(lhs, int)
        assert isinstance(rhs, int)
        return _predicate_word(lhs, "low32") | _predicate_word(rhs, "low32")

    if key == "amd.xdna.aie2p.predicate.or.low32.to.high32":
        lhs = operands["s0"]
        rhs = operands["s1"]
        storage = operands["storage"]
        assert isinstance(lhs, int)
        assert isinstance(rhs, int)
        assert isinstance(storage, int)
        high = _predicate_word(lhs, "low32") | _predicate_word(rhs, "low32")
        return (storage & _LOW_WORD_MASK) | (high << 32)

    if key == "amd.xdna.aie2p.predicate.or.high32":
        lhs = operands["s0"]
        rhs = operands["s1"]
        storage = operands["storage"]
        assert isinstance(lhs, int)
        assert isinstance(rhs, int)
        assert isinstance(storage, int)
        high = _predicate_word(lhs, "high32") | _predicate_word(rhs, "high32")
        return (storage & _LOW_WORD_MASK) | (high << 32)

    assert key == "amd.xdna.aie2p.predicate.complete.zero.high32"
    storage = operands["storage"]
    assert isinstance(storage, int)
    return storage & _LOW_WORD_MASK


def _evaluate_predicate_concat_rule(
    rule: DescriptorRule,
    left_bits: tuple[int, ...],
    right_bits: tuple[int, ...],
) -> tuple[int, ...]:
    left_lane_count = len(left_bits)
    right_lane_count = len(right_bits)
    values: dict[ValueRef, int | tuple[int, ...]] = {
        ValueRef.operand("inputs", element=0): _pack_predicate_bits(left_bits),
        ValueRef.operand("inputs", element=1): _pack_predicate_bits(right_bits),
    }
    for emit in rule.emit:
        if isinstance(emit, EmitRegisterSlice):
            source = values[emit.source]
            assert isinstance(source, tuple)
            assert emit.unit_count == 1
            values[emit.result] = source[emit.unit_offset]
            continue
        if isinstance(emit, EmitRegisterConcat):
            carriers = tuple(values[source] for source in emit.sources)
            assert all(isinstance(carrier, int) for carrier in carriers)
            values[emit.result] = carriers
            continue

        assert isinstance(emit, EmitDescriptorOp)
        operands = {name: values[value] for name, value in emit.operands.items()}
        result = _evaluate_descriptor(
            emit,
            operands,
            left_lane_count,
            right_lane_count,
        )
        assert len(emit.results) == 1
        values[next(iter(emit.results.values()))] = result

    result = values[ValueRef.result("result")]
    assert isinstance(result, (int, tuple))
    return _unpack_predicate_bits(result, left_lane_count + right_lane_count)


def _descriptor_keys(rule: DescriptorRule) -> list[str]:
    return [
        emit.descriptor.key for emit in rule.emit if isinstance(emit, EmitDescriptorOp)
    ]


def _assert_predicate_concat_partition(
    left_lane_count: int,
    right_lane_count: int,
) -> None:
    rule = _predicate_concat_rule_for_lanes(
        left_lane_count,
        right_lane_count,
    )
    left_bits = tuple(
        int((lane * 5 + left_lane_count) % 7 < 3) for lane in range(left_lane_count)
    )
    # Lane zero is always clear, so poisoned inactive left bits expose any
    # missing boundary mask immediately.
    right_bits = tuple(int(lane % 5 in (2, 4)) for lane in range(right_lane_count))
    assert (
        _evaluate_predicate_concat_rule(
            rule,
            left_bits,
            right_bits,
        )
        == left_bits + right_bits
    ), (left_lane_count, right_lane_count)


def test_predicate_concat_packs_one_word_directly() -> None:
    rule = _predicate_concat_rule_for_lanes(8, 8)
    assert _descriptor_keys(rule) == [
        "amd.xdna.aie2p.constant.i32.short",
        "amd.xdna.aie2p.constant.i32.mova",
        "amd.xdna.aie2p.predicate.mask.low32.to.low32",
        "amd.xdna.aie2p.predicate.shift.low32",
        "amd.xdna.aie2p.predicate.or.low32.rhs_tied",
        "amd.xdna.aie2p.predicate.complete.zero.high32",
    ]


def test_carrier_aligned_predicate_concat_is_structural() -> None:
    rule = _predicate_concat_rule_for_lanes(64, 64)
    assert rule.emit == (
        EmitRegisterConcat(
            sources=(
                ValueRef.operand("inputs", element=0),
                ValueRef.operand("inputs", element=1),
            ),
            result=ValueRef.result("result"),
        ),
    )


def test_predicate_concat_uses_bounded_scalar_word_composition() -> None:
    allowed_descriptors = {
        "amd.xdna.aie2p.constant.i32.short",
        "amd.xdna.aie2p.constant.i32.mova",
        "amd.xdna.aie2p.predicate.mask.low32.to.low32",
        "amd.xdna.aie2p.predicate.mask.high32.to.low32",
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
    for rule in AIE2P_PREDICATE_CONCAT_RULES:
        keys = _descriptor_keys(rule)
        assert set(keys) <= allowed_descriptors
        descriptor_counts.append(len(keys))

    # The hardest 128-lane composition remains a fixed sequence of scalar
    # predicate-word operations instead of expanding through vector bytes.
    assert max(descriptor_counts) == 15


def test_predicate_concat_matches_oracle_at_partition_boundaries() -> None:
    # These pairs cover both ends of every rule partition and each 32-bit and
    # 64-bit carrier boundary without making ordinary CI enumerate the domain.
    partitions = (
        (1, 1),
        (31, 1),
        (1, 31),
        (1, 32),
        (31, 33),
        (1, 63),
        (1, 64),
        (31, 64),
        (1, 65),
        (31, 65),
        (1, 127),
        (31, 97),
        (32, 1),
        (32, 32),
        (32, 33),
        (32, 64),
        (32, 65),
        (32, 96),
        (33, 1),
        (63, 1),
        (33, 31),
        (33, 32),
        (63, 2),
        (63, 64),
        (33, 65),
        (63, 65),
        (64, 1),
        (64, 64),
        (65, 1),
        (95, 1),
        (65, 31),
        (65, 32),
        (95, 33),
        (96, 1),
        (96, 32),
        (97, 1),
        (127, 1),
        (97, 31),
    )
    matched_rules = {
        id(_predicate_concat_rule_for_lanes(left, right)) for left, right in partitions
    }
    assert len(matched_rules) == len(AIE2P_PREDICATE_CONCAT_RULES)
    for left_lane_count, right_lane_count in partitions:
        _assert_predicate_concat_partition(left_lane_count, right_lane_count)


@pytest.mark.exhaustive
def test_predicate_concat_covers_and_evaluates_every_partition() -> None:
    for left_lane_count in range(1, 128):
        for right_lane_count in range(1, 129 - left_lane_count):
            _assert_predicate_concat_partition(
                left_lane_count,
                right_lane_count,
            )
