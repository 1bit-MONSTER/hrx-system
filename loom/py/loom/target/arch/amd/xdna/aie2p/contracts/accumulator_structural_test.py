# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Tests for AMD XDNA AIE2P accumulator structural contracts."""

from loom.target.arch.amd.xdna.aie2p.contracts.accumulator_structural import (
    _ACCUMULATOR_CONCAT_RULES,
    _ACCUMULATOR_CONCAT_TYPE_SPECS,
    _ACCUMULATOR_VECTOR_SHAPES,
    _accumulator_concat_left_shapes,
    _accumulator_concat_right_shapes,
)
from loom.target.contracts import (
    DescriptorRule,
    EmitDescriptorOp,
    EmitRegisterConcat,
    TypePattern,
    ValueRef,
    ValueTypeProject,
)


def _lane_bounds(pattern: TypePattern) -> tuple[int, int]:
    if pattern.lanes is not None:
        return pattern.lanes, pattern.lanes
    assert isinstance(pattern.minimum_lanes, int)
    assert isinstance(pattern.maximum_lanes, int)
    return pattern.minimum_lanes, pattern.maximum_lanes


def _matches_lanes(pattern: TypePattern, element_type: str, lane_count: int) -> bool:
    minimum, maximum = _lane_bounds(pattern)
    return pattern.element == element_type and minimum <= lane_count <= maximum


def _value_type_pattern(
    rule: DescriptorRule,
    field: str,
    *,
    element: int = 0,
) -> TypePattern:
    matches = tuple(
        guard.type_pattern
        for guard in rule.guards
        if guard.field == field
        and guard.element == element
        and guard.type_pattern is not None
    )
    assert len(matches) == 1
    return matches[0]


def _matching_concat_rules(
    element_type: str,
    left_lane_count: int,
    right_lane_count: int,
) -> tuple[DescriptorRule, ...]:
    result_lane_count = left_lane_count + right_lane_count
    return tuple(
        rule
        for rule in _ACCUMULATOR_CONCAT_RULES
        if _matches_lanes(
            _value_type_pattern(rule, "inputs", element=0),
            element_type,
            left_lane_count,
        )
        and _matches_lanes(
            _value_type_pattern(rule, "inputs", element=1),
            element_type,
            right_lane_count,
        )
        and _matches_lanes(
            _value_type_pattern(rule, "result"),
            element_type,
            result_lane_count,
        )
    )


def test_accumulator_shapes_use_only_allocatable_native_views() -> None:
    expected_units = {
        "f32": {32: 2, **{lane_count: 4 for lane_count in range(33, 65)}},
        "i32": {lane_count: 4 for lane_count in range(33, 65)},
        "i64": {lane_count: 4 for lane_count in range(17, 33)},
    }
    actual_units = {element_type: {} for element_type in expected_units}
    for shape in _ACCUMULATOR_VECTOR_SHAPES:
        for lane_count in range(
            shape.minimum_lane_count,
            shape.maximum_lane_count + 1,
        ):
            assert lane_count not in actual_units[shape.element_type]
            actual_units[shape.element_type][lane_count] = shape.register_unit_count
            expected_packet_count = (
                lane_count + shape.packet_lane_count - 1
            ) // shape.packet_lane_count
            assert expected_packet_count == shape.logical_packet_count
    assert actual_units == expected_units
    assert {shape.register_unit_count for shape in _ACCUMULATOR_VECTOR_SHAPES} == {
        2,
        4,
    }


def test_concat_operand_shapes_partition_each_logical_domain() -> None:
    for element_type, _, packet_lane_count in _ACCUMULATOR_CONCAT_TYPE_SPECS:
        for shapes in (
            _accumulator_concat_left_shapes(element_type, packet_lane_count),
            _accumulator_concat_right_shapes(element_type, packet_lane_count),
        ):
            for lane_count in range(1, 4 * packet_lane_count + 1):
                matches = tuple(
                    shape
                    for shape in shapes
                    if shape.minimum_lane_count
                    <= lane_count
                    <= shape.maximum_lane_count
                )
                assert len(matches) == 1
                shape = matches[0]
                assert (
                    shape.logical_packet_count
                    == (lane_count + packet_lane_count - 1) // packet_lane_count
                )
                expected_accumulator_units = (
                    2
                    if element_type == "f32" and lane_count == 2 * packet_lane_count
                    else 4
                    if lane_count > 2 * packet_lane_count
                    else None
                )
                assert shape.accumulator_unit_count == expected_accumulator_units


def test_accumulator_concat_rules_cover_every_binary_partition_once() -> None:
    result_domains = {
        "f32": range(32, 65),
        "i32": range(33, 65),
        "i64": range(17, 33),
    }
    for element_type, result_lane_counts in result_domains.items():
        for result_lane_count in result_lane_counts:
            for left_lane_count in range(1, result_lane_count):
                rules = _matching_concat_rules(
                    element_type,
                    left_lane_count,
                    result_lane_count - left_lane_count,
                )
                assert len(rules) == 1


def test_accumulator_concat_rules_fill_only_allocatable_carrier_units() -> None:
    for rule in _ACCUMULATOR_CONCAT_RULES:
        result_pattern = _value_type_pattern(rule, "result")
        result_minimum, result_maximum = _lane_bounds(result_pattern)
        shape = next(
            shape
            for shape in _ACCUMULATOR_VECTOR_SHAPES
            if shape.element_type == result_pattern.element
            and shape.minimum_lane_count <= result_minimum
            and result_maximum <= shape.maximum_lane_count
        )
        joined = rule.emit[-1]
        assert isinstance(joined, EmitRegisterConcat)
        if rule.descriptor is None:
            # The complete f32x32 pair concatenates two two-unit aggregates;
            # its dedicated test below checks that zero-instruction form.
            continue
        assert len(joined.sources) == shape.register_unit_count
        if shape.logical_packet_count < shape.register_unit_count:
            assert joined.sources[shape.logical_packet_count :] == (
                joined.sources[shape.logical_packet_count - 1],
            ) * (shape.register_unit_count - shape.logical_packet_count)


def test_partial_accumulator_concat_pads_only_the_physical_carrier() -> None:
    ordinary_boundary = _matching_concat_rules("i64", 16, 1)[0]
    joined = ordinary_boundary.emit[-1]
    assert isinstance(joined, EmitRegisterConcat)
    assert len(joined.sources) == 4
    assert joined.sources[-1] == joined.sources[-2]
    assert (
        sum(
            isinstance(emit, EmitDescriptorOp)
            and emit.descriptor.key == "amd.xdna.aie2p.move.vector512.to.accumulator512"
            for emit in ordinary_boundary.emit
        )
        == 3
    )

    f32_boundary = _matching_concat_rules("f32", 31, 1)[0]
    joined = f32_boundary.emit[-1]
    assert isinstance(joined, EmitRegisterConcat)
    assert len(joined.sources) == 2
    assert joined.sources[-1] != joined.sources[-2]


def test_partial_left_accumulator_repacks_only_the_logical_tail() -> None:
    rule = _matching_concat_rules("i64", 17, 1)[0]
    assert rule.descriptor is not None
    assert rule.descriptor.key == "amd.xdna.aie2p.shift.bytes.x.configured"
    descriptor_emits = tuple(
        emit for emit in rule.emit if isinstance(emit, EmitDescriptorOp)
    )
    assert [emit.descriptor.key for emit in descriptor_emits[:3]] == [
        "amd.xdna.aie2p.move.accumulator512.to.vector512",
        "amd.xdna.aie2p.constant.i32.mova",
        "amd.xdna.aie2p.shift.bytes.x.configured",
    ]
    assert descriptor_emits[1].immediates == {
        "i": ValueTypeProject.static_dim_scaled(
            ValueRef.operand("inputs", element=0),
            scale=8,
            addend=-128,
        )
    }
    remaining = next(
        emit
        for emit in descriptor_emits
        if emit.immediates
        == {
            "i": ValueTypeProject.literal_minus_static_dim_scaled(
                ValueRef.operand("inputs", element=0),
                scale=8,
                literal=192,
            )
        }
    )
    assert remaining.descriptor.key == "amd.xdna.aie2p.constant.i32.mova"
    joined = rule.emit[-1]
    assert isinstance(joined, EmitRegisterConcat)
    assert joined.sources[-1] == joined.sources[-2]


def test_complete_f32_accumulator_pair_remains_a_register_concat() -> None:
    rule = _matching_concat_rules("f32", 32, 32)[0]
    assert rule.descriptor is None
    assert rule.emit == (
        EmitRegisterConcat(
            sources=(
                ValueRef.operand("inputs", element=0),
                ValueRef.operand("inputs", element=1),
            ),
            result=ValueRef.result("result"),
        ),
    )
