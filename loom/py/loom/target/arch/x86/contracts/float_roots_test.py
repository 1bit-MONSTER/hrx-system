# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

from loom.dialect.scalar import math as scalar_math
from loom.dialect.vector import defs as vector
from loom.target.arch.x86.contracts.avx2 import X86_AVX2_CONTRACT_FRAGMENT
from loom.target.arch.x86.contracts.avx512 import X86_AVX512_CONTRACT_FRAGMENT
from loom.target.arch.x86.contracts.avx512_fp16 import (
    X86_AVX512_FP16_CONTRACT_FRAGMENT,
)
from loom.target.arch.x86.vector_families import (
    AVX2_ROOT_VECTOR_BIT_WIDTHS,
    AVX512_FP16_VECTOR_BIT_WIDTHS,
    AVX512_ROOT_VECTOR_BIT_WIDTHS,
)
from loom.target.contracts import (
    DescriptorRule,
    GuardKind,
    Scalar,
    ValueRef,
    Vector,
)

_ROOT_OPS = {
    scalar_math.scalar_sqrtf,
    scalar_math.scalar_rsqrtf,
    vector.vector_sqrtf,
    vector.vector_rsqrtf,
}


def _root_rules(fragment) -> tuple[DescriptorRule, ...]:
    return tuple(
        case
        for case in fragment.cases
        if isinstance(case, DescriptorRule) and case.source_op in _ROOT_OPS
    )


def _value_type(rule: DescriptorRule):
    return next(
        guard.type_pattern
        for guard in rule.guards
        if guard.kind == GuardKind.VALUE_TYPE and guard.field == "result"
    )


def _requires_afn(rule: DescriptorRule) -> bool:
    return any(
        guard.kind == GuardKind.INSTANCE_FLAGS_HAS_ALL
        and guard.field == "fastmath"
        and guard.enum_keyword == "afn"
        for guard in rule.guards
    )


def _cell(rule: DescriptorRule):
    return (
        rule.source_op.name,
        _value_type(rule),
        rule.descriptor.key,
        rule.priority,
        _requires_afn(rule),
    )


def test_avx2_root_contract_covers_scalar_and_each_logical_packet_width() -> None:
    assert {_cell(rule) for rule in _root_rules(X86_AVX2_CONTRACT_FRAGMENT)} == {
        ("scalar.sqrtf", Scalar("f32"), "x86.avx2.vsqrtss.xmm", 0, False),
        ("scalar.sqrtf", Scalar("f64"), "x86.avx2.vsqrtsd.xmm", 0, False),
        ("scalar.rsqrtf", Scalar("f32"), "x86.avx2.vrsqrtss.xmm", 0, True),
        *{
            (
                source_op,
                Vector(element, lanes=bit_width // element_width),
                f"x86.avx2.{mnemonic}.{'ymm' if bit_width == 256 else 'xmm'}",
                0,
                approximate,
            )
            for source_op, element, element_width, mnemonic, approximate in (
                ("vector.sqrtf", "f32", 32, "vsqrtps", False),
                ("vector.sqrtf", "f64", 64, "vsqrtpd", False),
                ("vector.rsqrtf", "f32", 32, "vrsqrtps", True),
            )
            for bit_width in AVX2_ROOT_VECTOR_BIT_WIDTHS
            if bit_width >= element_width
        },
    }


def test_avx512_root_overlay_covers_zmm_sqrt_and_every_rsqrt_width() -> None:
    assert {_cell(rule) for rule in _root_rules(X86_AVX512_CONTRACT_FRAGMENT)} == {
        (
            "scalar.rsqrtf",
            Scalar("f32"),
            "x86.avx512.vrsqrt14ss.xmm",
            1,
            True,
        ),
        (
            "scalar.rsqrtf",
            Scalar("f64"),
            "x86.avx512.vrsqrt14sd.xmm",
            1,
            True,
        ),
        (
            "vector.sqrtf",
            Vector("f32", lanes=16),
            "x86.avx512.vsqrtps.zmm",
            1,
            False,
        ),
        (
            "vector.sqrtf",
            Vector("f64", lanes=8),
            "x86.avx512.vsqrtpd.zmm",
            1,
            False,
        ),
        *{
            (
                "vector.rsqrtf",
                Vector(element, lanes=bit_width // element_width),
                f"x86.avx512.{mnemonic}.{register_suffix}",
                1,
                True,
            )
            for element, element_width, mnemonic in (
                ("f32", 32, "vrsqrt14ps"),
                ("f64", 64, "vrsqrt14pd"),
            )
            for bit_width in AVX512_ROOT_VECTOR_BIT_WIDTHS
            if bit_width >= element_width
            for register_suffix in (
                "zmm" if bit_width == 512 else "ymm" if bit_width == 256 else "xmm",
            )
        },
    }


def test_avx512_fp16_root_overlay_covers_scalar_and_every_packet_width() -> None:
    assert {_cell(rule) for rule in _root_rules(X86_AVX512_FP16_CONTRACT_FRAGMENT)} == {
        (
            f"scalar.{operation}",
            Scalar("f16"),
            f"x86.avx512_fp16.{mnemonic}.xmm",
            1,
            approximate,
        )
        for operation, mnemonic, approximate in (
            ("sqrtf", "vsqrtsh", False),
            ("rsqrtf", "vrsqrtsh", True),
        )
    } | {
        (
            f"vector.{operation}",
            Vector("f16", lanes=bit_width // 16),
            f"x86.avx512_fp16.{mnemonic}.{register_suffix}",
            1,
            approximate,
        )
        for operation, mnemonic, approximate in (
            ("sqrtf", "vsqrtph", False),
            ("rsqrtf", "vrsqrtph", True),
        )
        for bit_width in AVX512_FP16_VECTOR_BIT_WIDTHS
        for register_suffix in (
            "zmm" if bit_width == 512 else "ymm" if bit_width == 256 else "xmm",
        )
    }


def test_scalar_root_rules_preserve_upper_lanes_from_the_input() -> None:
    scalar_rules = tuple(
        rule
        for fragment in (
            X86_AVX2_CONTRACT_FRAGMENT,
            X86_AVX512_CONTRACT_FRAGMENT,
            X86_AVX512_FP16_CONTRACT_FRAGMENT,
        )
        for rule in _root_rules(fragment)
        if rule.source_op in (scalar_math.scalar_sqrtf, scalar_math.scalar_rsqrtf)
    )
    assert scalar_rules
    for rule in scalar_rules:
        assert len(rule.emit) == 1
        assert rule.emit[0].operands == {
            "passthrough": ValueRef.operand("input"),
            "input": ValueRef.operand("input"),
        }
