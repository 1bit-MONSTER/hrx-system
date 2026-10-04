# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Exact F32-to-narrow-float Wasm contract rules."""

from collections.abc import Callable

from loom.dialect.scalar import conversion as scalar_conversion
from loom.target.contracts import (
    DescriptorRule,
    Guard,
    Scalar,
    TypePattern,
    ValueRef,
)
from loom.target.emit.float_narrowing import (
    F8E4M3_FORMAT,
    F8E5M2_FORMAT,
    F16_FORMAT,
    F32_FORMAT,
    FloatNarrowingDescriptors,
    NarrowFloatFormat,
    NarrowFloatSubnormalRounding,
    build_f32_to_bf16_emits,
    build_float_to_narrow_float_emits,
)
from loom.target.low_descriptors import Descriptor

_F8E4M3 = Scalar("f8E4M3")
_F8E5M2 = Scalar("f8E5M2")
_F16 = Scalar("f16")
_BF16 = Scalar("bf16")
_F32 = Scalar("f32")


def _narrowing_descriptors(
    descriptor_lookup: Callable[[str], Descriptor],
) -> FloatNarrowingDescriptors:
    return FloatNarrowingDescriptors(
        integer_constant=descriptor_lookup("wasm.i32.const"),
        float_constant=descriptor_lookup("wasm.f32.const"),
        integer_add=descriptor_lookup("wasm.i32.add"),
        integer_subtract=descriptor_lookup("wasm.i32.sub"),
        integer_shift_left=descriptor_lookup("wasm.i32.shl"),
        integer_shift_right_logical=descriptor_lookup("wasm.i32.shr_u"),
        integer_bitwise_and=descriptor_lookup("wasm.i32.and"),
        integer_bitwise_or=descriptor_lookup("wasm.i32.or"),
        integer_less_than_nonnegative=descriptor_lookup("wasm.i32.lt_u"),
        integer_greater_than_equal_nonnegative=descriptor_lookup("wasm.i32.ge_u"),
        integer_greater_than_nonnegative=descriptor_lookup("wasm.i32.gt_u"),
        float_add=descriptor_lookup("wasm.f32.add"),
        reinterpret_float_as_integer=descriptor_lookup("wasm.i32.reinterpret_f32"),
        reinterpret_integer_as_float=descriptor_lookup("wasm.f32.reinterpret_i32"),
        integer_select=descriptor_lookup("wasm.i32.select"),
    )


def _f32_to_bf16_rule(
    descriptors: FloatNarrowingDescriptors,
    type_guard: Callable[[str, TypePattern], Guard],
) -> DescriptorRule:
    emits = build_f32_to_bf16_emits(
        descriptors,
        ValueRef.operand("input"),
        ValueRef.result("result"),
    )
    return DescriptorRule(
        source_op=scalar_conversion.scalar_fptrunc,
        descriptor=emits[-1].descriptor,
        guards=(
            type_guard("input", _F32),
            type_guard("result", _BF16),
        ),
        emit=emits,
        report_key="exact_binary32_to_bfloat16",
    )


def _f32_to_narrow_float_rule(
    descriptors: FloatNarrowingDescriptors,
    type_guard: Callable[[str, TypePattern], Guard],
    result_type: TypePattern,
    narrow_format: NarrowFloatFormat,
    report_key: str,
) -> DescriptorRule:
    emits = build_float_to_narrow_float_emits(
        descriptors,
        F32_FORMAT,
        narrow_format,
        ValueRef.operand("input"),
        ValueRef.result("result"),
        subnormal_rounding=NarrowFloatSubnormalRounding.RNE_FLOAT_ADD,
    )
    return DescriptorRule(
        source_op=scalar_conversion.scalar_fptrunc,
        descriptor=emits[-1].descriptor,
        guards=(
            type_guard("input", _F32),
            type_guard("result", result_type),
        ),
        emit=emits,
        report_key=report_key,
    )


def f32_narrowing_rules(
    descriptor_lookup: Callable[[str], Descriptor],
    type_guard: Callable[[str, TypePattern], Guard],
) -> tuple[DescriptorRule, ...]:
    """Returns exact F32-to-narrow-float rules for Wasm scalar carriers."""

    descriptors = _narrowing_descriptors(descriptor_lookup)
    return (
        _f32_to_bf16_rule(descriptors, type_guard),
        _f32_to_narrow_float_rule(
            descriptors,
            type_guard,
            _F16,
            F16_FORMAT,
            "exact_binary32_to_f16",
        ),
        _f32_to_narrow_float_rule(
            descriptors,
            type_guard,
            _F8E4M3,
            F8E4M3_FORMAT,
            "exact_binary32_to_f8e4m3",
        ),
        _f32_to_narrow_float_rule(
            descriptors,
            type_guard,
            _F8E5M2,
            F8E5M2_FORMAT,
            "exact_binary32_to_f8e5m2",
        ),
    )
