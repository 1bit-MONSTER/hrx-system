# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Semantic coverage for AIE2P dynamic vector windows."""

from functools import cache

import pytest

from loom.target.arch.amd.xdna.aie2p.contracts.carrier import (
    VECTOR_CARRIER_SPECS,
)
from loom.target.arch.amd.xdna.aie2p.contracts.dynamic_slice import (
    AIE2P_DYNAMIC_VECTOR_SLICE_RULES,
)
from loom.target.contracts import (
    DescriptorRule,
    EmitDescriptorOp,
    EmitRegisterConcat,
    EmitRegisterSlice,
    GuardKind,
    TypePattern,
    ValueRef,
)

_LOW_WORD_MASK = (1 << 32) - 1
_CARRIER_MASK = (1 << 64) - 1


def _pattern_matches(
    pattern: TypePattern,
    element_type: str,
    lane_count: int,
) -> bool:
    if element_type not in pattern.elements:
        return False
    if pattern.lanes is not None:
        return pattern.lanes == lane_count
    assert isinstance(pattern.minimum_lanes, int)
    assert isinstance(pattern.maximum_lanes, int)
    return pattern.minimum_lanes <= lane_count <= pattern.maximum_lanes


def _value_type_pattern(rule: DescriptorRule, field: str) -> TypePattern:
    matches = tuple(
        guard.type_pattern
        for guard in rule.guards
        if guard.kind is GuardKind.VALUE_TYPE
        and guard.field == field
        and guard.type_pattern is not None
    )
    assert len(matches) == 1
    return matches[0]


@cache
def _dynamic_slice_rule(
    element_type: str,
    source_lane_count: int,
    result_lane_count: int,
) -> DescriptorRule:
    matches = tuple(
        rule
        for rule in AIE2P_DYNAMIC_VECTOR_SLICE_RULES
        if _pattern_matches(
            _value_type_pattern(rule, "source"),
            element_type,
            source_lane_count,
        )
        and _pattern_matches(
            _value_type_pattern(rule, "result"),
            element_type,
            result_lane_count,
        )
    )
    assert len(matches) == 1, (
        element_type,
        source_lane_count,
        result_lane_count,
        matches,
    )
    return matches[0]


def _scalar_lshl(value: int, count: int) -> int:
    if count < 0:
        return (value & 0xFFFFFFFF) >> -count
    return (value << count) & 0xFFFFFFFF


def _element_byte_count(element_type: str) -> int:
    return {
        "i8": 1,
        "f8E4M3": 1,
        "f8E5M2": 1,
        "i16": 2,
        "f16": 2,
        "bf16": 2,
        "i32": 4,
        "f32": 4,
        "index": 4,
        "offset": 4,
        "i64": 8,
        "f64": 8,
    }[element_type]


def _uses_accumulator(element_type: str, lane_count: int) -> bool:
    return (
        (element_type == "f32" and lane_count >= 32)
        or (element_type in ("i32", "index", "offset") and lane_count >= 33)
        or (element_type == "i64" and lane_count >= 17)
    )


def _lane_bytes(element_byte_count: int, lane: int) -> bytes:
    value = (lane * 0x1F123BB5 + 0x1496C7) & ((1 << (8 * element_byte_count)) - 1)
    return value.to_bytes(element_byte_count, "little")


@cache
def _x_source_value(
    element_type: str,
    lane_count: int,
) -> tuple[bytes | tuple[bytes, ...], bytes]:
    element_byte_count = _element_byte_count(element_type)
    payload = b"".join(
        _lane_bytes(element_byte_count, lane) for lane in range(lane_count)
    )
    packet_count = (len(payload) + 63) // 64
    packets = tuple(
        payload[packet_index * 64 : (packet_index + 1) * 64].ljust(64, b"\xa5")
        for packet_index in range(packet_count)
    )
    if _uses_accumulator(element_type, lane_count):
        register_unit_count = 2 if element_type == "f32" and lane_count == 32 else 4
        return (
            (*packets, *((packets[-1],) * (register_unit_count - len(packets)))),
            payload,
        )
    if packet_count == 1:
        return packets[0], payload
    units = tuple(unit for packet in packets for unit in (packet[:32], packet[32:]))
    return units, payload


def _evaluate_x_rule(
    rule: DescriptorRule,
    source: bytes | tuple[bytes, ...],
    offset: int,
) -> bytes | tuple[bytes, ...]:
    values: dict[ValueRef, int | bytes | tuple[bytes, ...]] = {
        ValueRef.operand("source"): source,
        ValueRef.operand("offsets"): offset,
    }
    for emit in rule.emit:
        if isinstance(emit, EmitRegisterSlice):
            carrier = values[emit.source]
            assert isinstance(carrier, tuple)
            count = emit.unit_count or 1
            units = carrier[emit.unit_offset : emit.unit_offset + count]
            values[emit.result] = units[0] if count == 1 else b"".join(units)
            continue
        if isinstance(emit, EmitRegisterConcat):
            packets = tuple(values[source_ref] for source_ref in emit.sources)
            assert all(isinstance(packet, bytes) for packet in packets)
            values[emit.result] = packets
            continue

        assert isinstance(emit, EmitDescriptorOp)
        key = emit.descriptor.key
        operands = {name: values[value] for name, value in emit.operands.items()}
        if key == "amd.xdna.aie2p.constant.i32.short":
            value = emit.immediates["i"]
            assert isinstance(value, int)
        elif key == "amd.xdna.aie2p.lshl.i32":
            assert isinstance(operands["s0"], int)
            assert isinstance(operands["s1"], int)
            value = _scalar_lshl(operands["s0"], operands["s1"])
        elif key == "amd.xdna.aie2p.and.i32":
            assert isinstance(operands["s0"], int)
            assert isinstance(operands["s1"], int)
            value = operands["s0"] & operands["s1"]
        elif key == "amd.xdna.aie2p.select.mask.i32":
            assert isinstance(operands["s0"], int)
            value = -1 if operands["s0"] else 0
        elif key == "amd.xdna.aie2p.select.i32x16":
            assert isinstance(operands["s1"], bytes)
            assert isinstance(operands["s2"], bytes)
            assert isinstance(operands["sel"], int)
            value = operands["s1"] if operands["sel"] else operands["s2"]
        elif key in (
            "amd.xdna.aie2p.move.accumulator512.to.vector512",
            "amd.xdna.aie2p.move.vector512.to.accumulator512",
        ):
            value = operands["src"]
            assert isinstance(value, bytes)
        else:
            assert key == "amd.xdna.aie2p.shift.bytes.x.configured"
            low = operands["s1"]
            high = operands["s2"]
            shift = operands["shift"]
            assert isinstance(low, bytes)
            assert isinstance(high, bytes)
            assert isinstance(shift, int)
            value = (low + high)[shift : shift + 64]
        assert len(emit.results) == 1
        values[next(iter(emit.results.values()))] = value
    result = values[ValueRef.result("result")]
    assert isinstance(result, (bytes, tuple))
    return result


def _assert_x_slice(
    element_type: str,
    source_lane_count: int,
    offset: int,
    result_lane_count: int,
) -> None:
    rule = _dynamic_slice_rule(
        element_type,
        source_lane_count,
        result_lane_count,
    )
    source, payload = _x_source_value(element_type, source_lane_count)
    result = _evaluate_x_rule(rule, source, offset)
    result_bytes = result if isinstance(result, bytes) else b"".join(result)
    element_byte_count = _element_byte_count(element_type)
    expected = payload[
        offset * element_byte_count : (offset + result_lane_count) * element_byte_count
    ]
    assert result_bytes[: len(expected)] == expected, (
        element_type,
        source_lane_count,
        offset,
        result_lane_count,
    )


def _pack_predicate_bits(bits: tuple[int, ...]) -> int | tuple[int, ...]:
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


def _word(carrier: int, high: bool) -> int:
    return (carrier >> 32) & _LOW_WORD_MASK if high else carrier & _LOW_WORD_MASK


def _shift_word(word: int, shift: int) -> int:
    shifted = word << shift if shift >= 0 else word >> -shift
    return shifted & _LOW_WORD_MASK


def _evaluate_predicate_descriptor(
    emit: EmitDescriptorOp,
    operands: dict[str, int | bytes | tuple[int, ...]],
) -> int | bytes:
    key = emit.descriptor.key
    if key == "amd.xdna.aie2p.constant.i32.short":
        value = emit.immediates["i"]
        assert isinstance(value, int)
        return value
    if key == "amd.xdna.aie2p.lshl.i32":
        assert isinstance(operands["s0"], int)
        assert isinstance(operands["s1"], int)
        return _scalar_lshl(operands["s0"], operands["s1"])
    if key == "amd.xdna.aie2p.and.i32":
        assert isinstance(operands["s0"], int)
        assert isinstance(operands["s1"], int)
        return operands["s0"] & operands["s1"]
    if key == "amd.xdna.aie2p.sub.i32":
        assert isinstance(operands["s0"], int)
        assert isinstance(operands["s1"], int)
        return operands["s0"] - operands["s1"]
    if key == "amd.xdna.aie2p.splat.i8x64":
        source = operands["src"]
        assert isinstance(source, int)
        return bytes((source & 0xFF,)) * 64
    if key == "amd.xdna.aie2p.sub.i8x64":
        lhs = operands["s1"]
        rhs = operands["s2"]
        assert isinstance(lhs, bytes)
        assert isinstance(rhs, bytes)
        return bytes(
            (left - right) & 0xFF for left, right in zip(lhs, rhs, strict=True)
        )
    if key == "amd.xdna.aie2p.cmp.lt.unsigned.i8x64":
        lhs = operands["s1"]
        rhs = operands["s2"]
        assert isinstance(lhs, bytes)
        assert isinstance(rhs, bytes)
        return sum(
            int(left < right) << lane
            for lane, (left, right) in enumerate(zip(lhs, rhs, strict=True))
        )

    if ".predicate.shift." in key:
        source = operands["s0"]
        shift = operands["s1"]
        assert isinstance(source, int)
        assert isinstance(shift, int)
        shifted = _shift_word(_word(source, ".shift.high32" in key), shift)
        if not key.endswith(".to.high32"):
            return shifted
        storage = operands["storage"]
        assert isinstance(storage, int)
        return (storage & _LOW_WORD_MASK) | (shifted << 32)

    if ".predicate.xor.low32" in key:
        lhs = operands["s0"]
        rhs = operands["s1"]
        assert isinstance(lhs, int)
        assert isinstance(rhs, int)
        return _word(lhs, False) ^ _word(rhs, False)
    if key == "amd.xdna.aie2p.predicate.and.low32.rhs_tied":
        lhs = operands["s0"]
        rhs = operands["s1"]
        assert isinstance(lhs, int)
        assert isinstance(rhs, int)
        return _word(lhs, False) & _word(rhs, False)
    if key == "amd.xdna.aie2p.predicate.or.low32.rhs_tied":
        lhs = operands["s0"]
        rhs = operands["s1"]
        assert isinstance(lhs, int)
        assert isinstance(rhs, int)
        return _word(lhs, False) | _word(rhs, False)
    if key == "amd.xdna.aie2p.predicate.or.low32.to.high32":
        lhs = operands["s0"]
        rhs = operands["s1"]
        storage = operands["storage"]
        assert isinstance(lhs, int)
        assert isinstance(rhs, int)
        assert isinstance(storage, int)
        high = _word(lhs, False) | _word(rhs, False)
        return (storage & _LOW_WORD_MASK) | (high << 32)

    assert key == "amd.xdna.aie2p.predicate.complete.zero.high32"
    storage = operands["storage"]
    assert isinstance(storage, int)
    return storage & _LOW_WORD_MASK


def _evaluate_predicate_rule(
    rule: DescriptorRule,
    source_bits: tuple[int, ...],
    offset: int,
    result_lane_count: int,
) -> tuple[int, ...]:
    values: dict[ValueRef, int | bytes | tuple[int, ...]] = {
        ValueRef.operand("source"): _pack_predicate_bits(source_bits),
        ValueRef.operand("offsets"): offset,
    }
    for emit in rule.emit:
        if isinstance(emit, EmitRegisterSlice):
            carrier = values[emit.source]
            assert isinstance(carrier, tuple)
            assert emit.unit_count == 1
            values[emit.result] = carrier[emit.unit_offset]
            continue
        if isinstance(emit, EmitRegisterConcat):
            carriers = tuple(values[source] for source in emit.sources)
            assert all(isinstance(carrier, int) for carrier in carriers)
            values[emit.result] = carriers
            continue

        assert isinstance(emit, EmitDescriptorOp)
        operands = {name: values[value] for name, value in emit.operands.items()}
        result = _evaluate_predicate_descriptor(emit, operands)
        assert len(emit.results) == 1
        values[next(iter(emit.results.values()))] = result
    packed_result = values[ValueRef.result("result")]
    assert isinstance(packed_result, (int, tuple))
    return _unpack_predicate_bits(packed_result, result_lane_count)


def _assert_predicate_slice(
    source_lane_count: int,
    offset: int,
    result_lane_count: int,
) -> None:
    rule = _dynamic_slice_rule("i1", source_lane_count, result_lane_count)
    source_bits = _predicate_source_bits(source_lane_count)
    assert (
        _evaluate_predicate_rule(
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


@cache
def _predicate_source_bits(source_lane_count: int) -> tuple[int, ...]:
    return tuple(
        int((lane * 13 + source_lane_count) % 17 < 8)
        for lane in range(source_lane_count)
    )


def _boundary_values(limit: int, packet_lane_count: int) -> tuple[int, ...]:
    packet_boundaries = {
        value
        for packet_base in range(
            packet_lane_count, limit + packet_lane_count, packet_lane_count
        )
        for value in (packet_base - 1, packet_base, packet_base + 1)
    }
    return tuple(
        sorted(
            value
            for value in {
                0,
                1,
                *packet_boundaries,
                limit - 1,
                limit,
            }
            if 0 <= value <= limit
        )
    )


def test_dynamic_slice_rules_partition_the_complete_carrier_family() -> None:
    assert len(AIE2P_DYNAMIC_VECTOR_SLICE_RULES) == 64
    for rule in AIE2P_DYNAMIC_VECTOR_SLICE_RULES:
        dynamic_offset_guard = rule.guards[0]
        assert dynamic_offset_guard.kind is GuardKind.OPERAND_SEGMENT_COUNT
        assert dynamic_offset_guard.field == "offsets"
        assert dynamic_offset_guard.count == 1
        static_offset_guard = rule.guards[1]
        assert static_offset_guard.kind is GuardKind.I64_ARRAY_COUNT
        assert static_offset_guard.field == "static_offsets"
        assert static_offset_guard.count == 1
        assert not any(
            guard.kind is GuardKind.I64_ARRAY_ELEMENT_RANGE
            and guard.field == "static_offsets"
            for guard in rule.guards
        )
        descriptor_count = sum(isinstance(emit, EmitDescriptorOp) for emit in rule.emit)
        assert descriptor_count <= 50, (descriptor_count, rule)


def test_dynamic_x_slice_matches_every_carrier_boundary() -> None:
    ordinary_types = tuple(
        (element_type, element_byte_count, maximum_lane_count)
        for element_types, element_byte_count, maximum_lane_count in (
            VECTOR_CARRIER_SPECS
        )
        for element_type in element_types
    )
    for element_type, element_byte_count, maximum_lane_count in ordinary_types:
        packet_lane_count = 64 // element_byte_count
        source_lane_counts = _boundary_values(
            maximum_lane_count,
            packet_lane_count,
        )[1:]
        for source_lane_count in source_lane_counts:
            for result_lane_count in _boundary_values(
                source_lane_count,
                packet_lane_count,
            )[1:]:
                maximum_offset = source_lane_count - result_lane_count
                for offset in _boundary_values(maximum_offset, packet_lane_count):
                    _assert_x_slice(
                        element_type,
                        source_lane_count,
                        offset,
                        result_lane_count,
                    )

    accumulator_domains = {
        "f32": ((32, 33, 48, 49, 64), 16),
        "i32": ((33, 48, 49, 64), 16),
        "index": ((33, 48, 49, 64), 16),
        "offset": ((33, 48, 49, 64), 16),
        "i64": ((17, 24, 25, 32), 8),
    }
    for element_type, (
        source_lane_counts,
        packet_lane_count,
    ) in accumulator_domains.items():
        for source_lane_count in source_lane_counts:
            for result_lane_count in _boundary_values(
                source_lane_count,
                packet_lane_count,
            )[1:]:
                maximum_offset = source_lane_count - result_lane_count
                for offset in _boundary_values(maximum_offset, packet_lane_count):
                    _assert_x_slice(
                        element_type,
                        source_lane_count,
                        offset,
                        result_lane_count,
                    )


def test_dynamic_predicate_slice_matches_every_word_boundary() -> None:
    for source_lane_count in _boundary_values(128, 32)[1:]:
        for result_lane_count in _boundary_values(source_lane_count, 32)[1:]:
            maximum_offset = source_lane_count - result_lane_count
            for offset in _boundary_values(maximum_offset, 32):
                _assert_predicate_slice(
                    source_lane_count,
                    offset,
                    result_lane_count,
                )


@pytest.mark.exhaustive
def test_dynamic_slice_exhausts_every_supported_interval() -> None:
    for element_type, maximum_lane_count in (
        ("i8", 128),
        ("i16", 64),
        ("i32", 64),
        ("f32", 64),
        ("i64", 32),
    ):
        for source_lane_count in range(1, maximum_lane_count + 1):
            accumulator_minimum = {"i32": 33, "f32": 32, "i64": 17}.get(element_type)
            if (
                accumulator_minimum is not None
                and source_lane_count >= accumulator_minimum
            ):
                supported = True
            else:
                supported = any(
                    element_type in element_types
                    and source_lane_count <= ordinary_maximum
                    for element_types, _, ordinary_maximum in VECTOR_CARRIER_SPECS
                )
            if not supported:
                continue
            for result_lane_count in range(1, source_lane_count + 1):
                for offset in range(source_lane_count - result_lane_count + 1):
                    _assert_x_slice(
                        element_type,
                        source_lane_count,
                        offset,
                        result_lane_count,
                    )

    for source_lane_count in range(1, 129):
        for result_lane_count in range(1, source_lane_count + 1):
            for offset in range(source_lane_count - result_lane_count + 1):
                _assert_predicate_slice(
                    source_lane_count,
                    offset,
                    result_lane_count,
                )
