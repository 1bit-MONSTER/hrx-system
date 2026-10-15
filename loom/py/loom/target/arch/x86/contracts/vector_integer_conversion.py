# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""x86 packed-integer vector conversion contract rules."""

from __future__ import annotations

from loom.dialect.vector import defs as vector
from loom.target.arch.x86.contracts.rule_builders import (
    DescriptorLookup as _DescriptorLookup,
)
from loom.target.arch.x86.contracts.rule_builders import emit_descriptor_op as _op_emit
from loom.target.arch.x86.vector_families import (
    AVX2_CARRIER_VECTOR_BIT_WIDTHS,
    AVX512_CARRIER_VECTOR_BIT_WIDTHS,
    X86_VECTOR_INTEGER_NARROWING_FAMILIES,
    X86_VECTOR_INTEGER_WIDENING_FAMILIES,
    VectorIntegerNarrowingFamily,
)
from loom.target.contracts import (
    DescriptorResultType,
    DescriptorRule,
    Guard,
    ReadOnlyDataLiteral,
    ValueRef,
    Vector,
)

_REGISTER_SUFFIXES = {128: "xmm", 256: "ymm", 512: "zmm"}
_AVX2_NARROW_JOIN_MNEMONICS = {
    16: "vpunpcklwd",
    32: "vpunpckldq",
    64: "vpunpcklqdq",
}


def _physical_register_suffix(vector_bit_width: int) -> str:
    return _REGISTER_SUFFIXES[max(128, vector_bit_width)]


def vector_integer_widening_rules(
    descriptor_lookup: _DescriptorLookup,
    *,
    descriptor_key_prefix: str,
    source_vector_bit_widths: tuple[int, ...],
    result_vector_bit_widths: tuple[int, ...],
) -> tuple[DescriptorRule, ...]:
    """Returns native rules between supported one-register vector carriers."""

    rules: list[DescriptorRule] = []
    for family in X86_VECTOR_INTEGER_WIDENING_FAMILIES:
        for source_op, mnemonic in (
            (vector.vector_extsi, family.signed_mnemonic),
            (vector.vector_extui, family.unsigned_mnemonic),
        ):
            for result_vector_bit_width in result_vector_bit_widths:
                if result_vector_bit_width < family.result.bit_width:
                    continue
                lane_count = result_vector_bit_width // family.result.bit_width
                source_vector_bit_width = lane_count * family.source.bit_width
                if source_vector_bit_width not in source_vector_bit_widths:
                    continue
                source_type = Vector(family.source.name, lanes=lane_count)
                result_type = Vector(family.result.name, lanes=lane_count)
                source_suffix = _physical_register_suffix(source_vector_bit_width)
                descriptor = descriptor_lookup(
                    f"{descriptor_key_prefix}.{mnemonic}."
                    f"{_physical_register_suffix(result_vector_bit_width)}."
                    f"{source_suffix}"
                )
                rules.append(
                    DescriptorRule(
                        source_op=source_op,
                        descriptor=descriptor,
                        guards=(
                            Guard.value_type("input", source_type),
                            Guard.value_type("result", result_type),
                            Guard.value_static_element_count_eq("input", "result"),
                        ),
                        emit=(
                            _op_emit(
                                descriptor=descriptor,
                                operands={"source": ValueRef.operand("input")},
                                results={"dst": ValueRef.result("result")},
                            ),
                        ),
                        report_key="x86.vector_integer_widen.native",
                    )
                )
    return tuple(rules)


def _avx2_narrowing_mask(
    family: VectorIntegerNarrowingFamily,
    source_vector_bit_width: int,
) -> bytes:
    """Builds per-128-bit-lane PSHUFB controls for one logical source."""

    physical_vector_bit_width = max(128, source_vector_bit_width)
    source_bytes = family.source.bit_width // 8
    result_bytes = family.result.bit_width // 8
    mask = bytearray()
    for lane_bit_offset in range(0, physical_vector_bit_width, 128):
        logical_lane_bit_width = min(
            128, max(0, source_vector_bit_width - lane_bit_offset)
        )
        lane_count = logical_lane_bit_width // family.source.bit_width
        lane_mask = [
            lane * source_bytes + byte
            for lane in range(lane_count)
            for byte in range(result_bytes)
        ]
        mask.extend(lane_mask)
        mask.extend([0x80] * (16 - len(lane_mask)))
    return bytes(mask)


def _avx2_vector_integer_narrowing_rule(
    family: VectorIntegerNarrowingFamily,
    source_vector_bit_width: int,
    descriptor_lookup: _DescriptorLookup,
) -> DescriptorRule:
    lane_count = source_vector_bit_width // family.source.bit_width
    source_type = Vector(family.source.name, lanes=lane_count)
    result_type = Vector(family.result.name, lanes=lane_count)
    physical_source_bit_width = max(128, source_vector_bit_width)
    source_suffix = _physical_register_suffix(source_vector_bit_width)
    load = descriptor_lookup(f"x86.avx2.vmovdqu.rodata.{source_suffix}")
    shuffle = descriptor_lookup(f"x86.avx2.vpshufb.{source_suffix}")
    control = ValueRef.temporary("control")
    emits = [
        _op_emit(
            descriptor=load,
            results={"dst": control},
            result_types={"dst": DescriptorResultType()},
            immediates={
                "data": ReadOnlyDataLiteral(
                    _avx2_narrowing_mask(family, source_vector_bit_width)
                )
            },
        )
    ]
    required_descriptors = [load]
    if physical_source_bit_width == 128:
        emits.append(
            _op_emit(
                descriptor=shuffle,
                operands={"lhs": ValueRef.operand("input"), "rhs": control},
                results={"dst": ValueRef.result("result")},
            )
        )
    else:
        packed = ValueRef.temporary("packed")
        emits.append(
            _op_emit(
                descriptor=shuffle,
                operands={"lhs": ValueRef.operand("input"), "rhs": control},
                results={"dst": packed},
                result_types={"dst": DescriptorResultType()},
            )
        )
        extract = descriptor_lookup("x86.avx2.vextractf128.xmm.ymm")
        halves = tuple(ValueRef.temporary(f"half{lane}") for lane in range(2))
        for lane, half in enumerate(halves):
            emits.append(
                _op_emit(
                    descriptor=extract,
                    operands={"source": packed},
                    results={"dst": half},
                    result_types={"dst": DescriptorResultType()},
                    immediates={"lane": lane},
                )
            )
        join_mnemonic = _AVX2_NARROW_JOIN_MNEMONICS[family.avx2_half_result_bit_width]
        join = descriptor_lookup(f"x86.avx2.{join_mnemonic}.xmm")
        emits.append(
            _op_emit(
                descriptor=join,
                operands={"lhs": halves[0], "rhs": halves[1]},
                results={"dst": ValueRef.result("result")},
            )
        )
        required_descriptors.extend((extract, join))

    return DescriptorRule(
        source_op=vector.vector_trunci,
        descriptor=shuffle,
        guards=(
            Guard.value_type("input", source_type),
            Guard.value_type("result", result_type),
            Guard.value_static_element_count_eq("input", "result"),
            *(Guard.descriptor_available(row) for row in required_descriptors),
        ),
        emit=tuple(emits),
        report_key="x86.vector_integer_narrow.avx2_shuffle",
    )


def avx2_vector_integer_narrowing_rules(
    descriptor_lookup: _DescriptorLookup,
) -> tuple[DescriptorRule, ...]:
    """Returns vectorized AVX2 rules for every integer narrowing pair."""

    return tuple(
        _avx2_vector_integer_narrowing_rule(
            family, source_vector_bit_width, descriptor_lookup
        )
        for family in X86_VECTOR_INTEGER_NARROWING_FAMILIES
        for source_vector_bit_width in AVX2_CARRIER_VECTOR_BIT_WIDTHS
        if source_vector_bit_width >= family.source.bit_width
        if (
            source_vector_bit_width * family.result.bit_width // family.source.bit_width
            in AVX2_CARRIER_VECTOR_BIT_WIDTHS
        )
    )


def avx512_vector_integer_narrowing_rules(
    descriptor_lookup: _DescriptorLookup,
) -> tuple[DescriptorRule, ...]:
    """Returns native AVX-512 rules for every integer narrowing pair."""

    rules: list[DescriptorRule] = []
    for family in X86_VECTOR_INTEGER_NARROWING_FAMILIES:
        for source_vector_bit_width in AVX512_CARRIER_VECTOR_BIT_WIDTHS:
            if source_vector_bit_width < family.source.bit_width:
                continue
            lane_count = source_vector_bit_width // family.source.bit_width
            result_vector_bit_width = lane_count * family.result.bit_width
            if result_vector_bit_width not in AVX512_CARRIER_VECTOR_BIT_WIDTHS:
                continue
            physical_source_bit_width = max(128, source_vector_bit_width)
            physical_result_bit_width = max(
                128,
                result_vector_bit_width,
            )
            descriptor = descriptor_lookup(
                f"x86.avx512.{family.mnemonic}."
                f"{_physical_register_suffix(physical_result_bit_width)}."
                f"{_physical_register_suffix(physical_source_bit_width)}"
            )
            rules.append(
                DescriptorRule(
                    source_op=vector.vector_trunci,
                    descriptor=descriptor,
                    guards=(
                        Guard.value_type(
                            "input", Vector(family.source.name, lanes=lane_count)
                        ),
                        Guard.value_type(
                            "result", Vector(family.result.name, lanes=lane_count)
                        ),
                        Guard.value_static_element_count_eq("input", "result"),
                    ),
                    emit=(
                        _op_emit(
                            descriptor=descriptor,
                            operands={"source": ValueRef.operand("input")},
                            results={"dst": ValueRef.result("result")},
                        ),
                    ),
                    priority=1,
                    report_key="x86.vector_integer_narrow.native",
                )
            )
    return tuple(rules)
