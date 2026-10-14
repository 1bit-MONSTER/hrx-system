# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

import pytest

from loom.dialect.vector import defs as vector
from loom.target.arch.x86.contracts.vector_integer_conversion import (
    vector_integer_widening_rules,
)
from loom.target.arch.x86.descriptors import (
    X86_AVX2_DESCRIPTOR_SET,
    X86_AVX512_CORE_DESCRIPTOR_SET,
)
from loom.target.arch.x86.vector_families import (
    AVX2_CARRIER_VECTOR_BIT_WIDTHS,
    AVX512_CARRIER_VECTOR_BIT_WIDTHS,
    X86_VECTOR_INTEGER_WIDENING_FAMILIES,
)
from loom.target.contracts import GuardKind, ValueRef, Vector, descriptor_by_key
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
