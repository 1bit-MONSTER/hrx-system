# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

import pytest

from loom.dialect.vector import defs as vector
from loom.target.arch.x86.contracts.vector_integer_conversion import (
    avx2_vector_integer_narrowing_rules,
    avx512_vector_integer_narrowing_rules,
    vector_integer_widening_rules,
)
from loom.target.arch.x86.descriptors import (
    X86_AVX2_DESCRIPTOR_SET,
    X86_AVX512_CORE_DESCRIPTOR_SET,
)
from loom.target.arch.x86.vector_families import (
    AVX2_CARRIER_VECTOR_BIT_WIDTHS,
    AVX512_CARRIER_VECTOR_BIT_WIDTHS,
    X86_VECTOR_INTEGER_NARROWING_FAMILIES,
    X86_VECTOR_INTEGER_WIDENING_FAMILIES,
)
from loom.target.contracts import (
    DescriptorRule,
    GuardKind,
    ReadOnlyDataLiteral,
    TypePattern,
    ValueRef,
    Vector,
    descriptor_by_key,
)
from loom.target.low_descriptors import DescriptorSet


def _expected_widening_cells(
    *,
    descriptor_prefix: str,
    source_bit_widths: tuple[int, ...],
    result_bit_widths: tuple[int, ...],
) -> set[tuple[object, object, object, str]]:
    register_suffixes = {128: "xmm", 256: "ymm", 512: "zmm"}
    return {
        (
            source_op,
            Vector(family.source.name, lanes=lane_count),
            Vector(family.result.name, lanes=lane_count),
            (
                f"{descriptor_prefix}.{mnemonic}."
                f"{register_suffixes[max(128, result_bit_width)]}."
                f"{register_suffixes[max(128, source_bit_width)]}"
            ),
        )
        for family in X86_VECTOR_INTEGER_WIDENING_FAMILIES
        for source_op, mnemonic in (
            (vector.vector_extsi, family.signed_mnemonic),
            (vector.vector_extui, family.unsigned_mnemonic),
        )
        for result_bit_width in result_bit_widths
        if result_bit_width >= family.result.bit_width
        for lane_count in (result_bit_width // family.result.bit_width,)
        for source_bit_width in (lane_count * family.source.bit_width,)
        if source_bit_width in source_bit_widths
    }


@pytest.mark.parametrize(
    (
        "descriptor_set",
        "descriptor_prefix",
        "source_bit_widths",
        "result_bit_widths",
    ),
    [
        (
            X86_AVX2_DESCRIPTOR_SET,
            "x86.avx2",
            AVX2_CARRIER_VECTOR_BIT_WIDTHS,
            AVX2_CARRIER_VECTOR_BIT_WIDTHS,
        ),
        (
            X86_AVX512_CORE_DESCRIPTOR_SET,
            "x86.avx512",
            AVX512_CARRIER_VECTOR_BIT_WIDTHS,
            (512,),
        ),
    ],
)
def test_widening_rules_cover_every_one_register_source_and_result(
    descriptor_set: DescriptorSet,
    descriptor_prefix: str,
    source_bit_widths: tuple[int, ...],
    result_bit_widths: tuple[int, ...],
) -> None:
    rules = vector_integer_widening_rules(
        lambda key: descriptor_by_key(descriptor_set, key),
        descriptor_key_prefix=descriptor_prefix,
        source_vector_bit_widths=source_bit_widths,
        result_vector_bit_widths=result_bit_widths,
    )

    actual = set()
    for rule in rules:
        type_guards = {
            guard.field: guard.type_pattern
            for guard in rule.guards
            if guard.kind is GuardKind.VALUE_TYPE
        }
        shape_guards = tuple(
            guard
            for guard in rule.guards
            if guard.kind is GuardKind.VALUE_STATIC_ELEMENT_COUNT_EQ
        )
        assert len(shape_guards) == 1
        assert (shape_guards[0].field, shape_guards[0].other_field) == (
            "input",
            "result",
        )
        assert len(rule.emit) == 1
        assert rule.emit[0].descriptor is rule.descriptor
        assert rule.emit[0].operands == {"source": ValueRef.operand("input")}
        assert rule.emit[0].results == {"dst": ValueRef.result("result")}
        assert rule.report_key == "x86.vector_integer_widen.native"
        actual.add(
            (
                rule.source_op,
                type_guards["input"],
                type_guards["result"],
                rule.descriptor.key,
            )
        )

    assert actual == _expected_widening_cells(
        descriptor_prefix=descriptor_prefix,
        source_bit_widths=source_bit_widths,
        result_bit_widths=result_bit_widths,
    )


def _type_guards(rule: DescriptorRule) -> dict[str, TypePattern]:
    return {
        guard.field: guard.type_pattern
        for guard in rule.guards
        if guard.kind is GuardKind.VALUE_TYPE
    }


def _expected_avx2_mask(
    source_vector_bit_width: int,
    source_element_bit_width: int,
    result_element_bit_width: int,
) -> bytes:
    physical_vector_bit_width = max(128, source_vector_bit_width)
    mask = bytearray()
    for lane_bit_offset in range(0, physical_vector_bit_width, 128):
        logical_lane_bit_width = min(
            128, max(0, source_vector_bit_width - lane_bit_offset)
        )
        lane_count = logical_lane_bit_width // source_element_bit_width
        lane_mask = [
            lane * (source_element_bit_width // 8) + byte
            for lane in range(lane_count)
            for byte in range(result_element_bit_width // 8)
        ]
        mask.extend(lane_mask)
        mask.extend([0x80] * (16 - len(lane_mask)))
    return bytes(mask)


def test_avx2_narrowing_rules_cover_every_pair_width_mask_and_join() -> None:
    rules = avx2_vector_integer_narrowing_rules(
        lambda key: descriptor_by_key(X86_AVX2_DESCRIPTOR_SET, key)
    )
    expected_cells = {
        (
            family.source.name,
            family.result.name,
            source_vector_bit_width,
        )
        for family in X86_VECTOR_INTEGER_NARROWING_FAMILIES
        for source_vector_bit_width in AVX2_CARRIER_VECTOR_BIT_WIDTHS
        if source_vector_bit_width >= family.source.bit_width
        if (
            source_vector_bit_width * family.result.bit_width // family.source.bit_width
            in AVX2_CARRIER_VECTOR_BIT_WIDTHS
        )
    }
    family_by_elements = {
        (family.source.name, family.result.name): family
        for family in X86_VECTOR_INTEGER_NARROWING_FAMILIES
    }
    actual_cells = set()
    for rule in rules:
        guards = _type_guards(rule)
        source_type = guards["input"]
        result_type = guards["result"]
        family = family_by_elements[(source_type.element, result_type.element)]
        source_vector_bit_width = source_type.lanes * family.source.bit_width
        physical_source_bit_width = max(128, source_vector_bit_width)
        register_suffix = "xmm" if physical_source_bit_width == 128 else "ymm"
        assert rule.source_op is vector.vector_trunci
        assert rule.descriptor.key == f"x86.avx2.vpshufb.{register_suffix}"
        assert rule.priority == 0
        assert rule.report_key == "x86.vector_integer_narrow.avx2_shuffle"
        assert result_type.lanes == source_type.lanes

        literal = rule.emit[0].immediates["data"]
        assert isinstance(literal, ReadOnlyDataLiteral)
        assert literal.data == _expected_avx2_mask(
            source_vector_bit_width,
            family.source.bit_width,
            family.result.bit_width,
        )
        assert rule.emit[0].descriptor.key == (
            f"x86.avx2.vmovdqu.rodata.{register_suffix}"
        )
        descriptor_keys = tuple(emit.descriptor.key for emit in rule.emit)
        if physical_source_bit_width == 128:
            assert descriptor_keys == (
                "x86.avx2.vmovdqu.rodata.xmm",
                "x86.avx2.vpshufb.xmm",
            )
        else:
            join_mnemonic = {
                16: "vpunpcklwd",
                32: "vpunpckldq",
                64: "vpunpcklqdq",
            }[family.avx2_half_result_bit_width]
            assert descriptor_keys == (
                "x86.avx2.vmovdqu.rodata.ymm",
                "x86.avx2.vpshufb.ymm",
                "x86.avx2.vextractf128.xmm.ymm",
                "x86.avx2.vextractf128.xmm.ymm",
                f"x86.avx2.{join_mnemonic}.xmm",
            )
            assert tuple(emit.immediates.get("lane") for emit in rule.emit[2:4]) == (
                0,
                1,
            )
        assert rule.emit[-1].results == {"dst": ValueRef.result("result")}
        actual_cells.add(
            (
                family.source.name,
                family.result.name,
                source_vector_bit_width,
            )
        )

    assert actual_cells == expected_cells
    assert len(rules) == len(expected_cells)


def test_avx512_narrowing_rules_cover_every_pair_and_carrier_width() -> None:
    rules = avx512_vector_integer_narrowing_rules(
        lambda key: descriptor_by_key(X86_AVX512_CORE_DESCRIPTOR_SET, key)
    )
    expected_cells = {
        (
            family.source.name,
            family.result.name,
            source_vector_bit_width,
        )
        for family in X86_VECTOR_INTEGER_NARROWING_FAMILIES
        for source_vector_bit_width in AVX512_CARRIER_VECTOR_BIT_WIDTHS
        if source_vector_bit_width >= family.source.bit_width
        if (
            source_vector_bit_width * family.result.bit_width // family.source.bit_width
            in AVX512_CARRIER_VECTOR_BIT_WIDTHS
        )
    }
    family_by_elements = {
        (family.source.name, family.result.name): family
        for family in X86_VECTOR_INTEGER_NARROWING_FAMILIES
    }
    actual_cells = set()
    suffixes = {128: "xmm", 256: "ymm", 512: "zmm"}
    for rule in rules:
        guards = _type_guards(rule)
        source_type = guards["input"]
        result_type = guards["result"]
        family = family_by_elements[(source_type.element, result_type.element)]
        source_vector_bit_width = source_type.lanes * family.source.bit_width
        physical_source_bit_width = max(128, source_vector_bit_width)
        physical_result_bit_width = max(
            128,
            physical_source_bit_width
            * family.result.bit_width
            // family.source.bit_width,
        )
        assert rule.source_op is vector.vector_trunci
        assert rule.descriptor.key == (
            f"x86.avx512.{family.mnemonic}."
            f"{suffixes[physical_result_bit_width]}."
            f"{suffixes[physical_source_bit_width]}"
        )
        assert rule.priority == 1
        assert rule.report_key == "x86.vector_integer_narrow.native"
        assert len(rule.emit) == 1
        assert rule.emit[0].operands == {"source": ValueRef.operand("input")}
        assert rule.emit[0].results == {"dst": ValueRef.result("result")}
        actual_cells.add(
            (
                family.source.name,
                family.result.name,
                source_vector_bit_width,
            )
        )

    assert actual_cells == expected_cells
    assert len(rules) == len(expected_cells)
