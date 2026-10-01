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
from loom.target.emit.f32_narrowing import (
    F8E4M3_FORMAT,
    F8E5M2_FORMAT,
    F16_FORMAT,
    F32NarrowingDescriptors,
    F32NarrowingFormat,
    build_f32_to_bf16_emits,
    build_f32_to_narrow_float_emits,
)
from loom.target.low_descriptors import Descriptor

_F8E4M3 = Scalar("f8E4M3")
_F8E5M2 = Scalar("f8E5M2")
_F16 = Scalar("f16")
_BF16 = Scalar("bf16")
_F32 = Scalar("f32")


def _narrowing_descriptors(
    descriptor_lookup: Callable[[str], Descriptor],
) -> F32NarrowingDescriptors:
    return F32NarrowingDescriptors(
        i32_constant=descriptor_lookup("wasm.i32.const"),
        f32_constant=descriptor_lookup("wasm.f32.const"),
        i32_add=descriptor_lookup("wasm.i32.add"),
        i32_subtract=descriptor_lookup("wasm.i32.sub"),
        i32_shift_right_logical=descriptor_lookup("wasm.i32.shr_u"),
        i32_bitwise_and=descriptor_lookup("wasm.i32.and"),
        i32_bitwise_or=descriptor_lookup("wasm.i32.or"),
        i32_less_than_nonnegative=descriptor_lookup("wasm.i32.lt_u"),
        i32_greater_than_equal_nonnegative=descriptor_lookup("wasm.i32.ge_u"),
        i32_greater_than_nonnegative=descriptor_lookup("wasm.i32.gt_u"),
        f32_add=descriptor_lookup("wasm.f32.add"),
        reinterpret_f32_as_i32=descriptor_lookup("wasm.i32.reinterpret_f32"),
        reinterpret_i32_as_f32=descriptor_lookup("wasm.f32.reinterpret_i32"),
        i32_select=descriptor_lookup("wasm.i32.select"),
    )


def _f32_to_bf16_rule(
    descriptors: F32NarrowingDescriptors,
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
    descriptors: F32NarrowingDescriptors,
    type_guard: Callable[[str, TypePattern], Guard],
    result_type: TypePattern,
    narrow_format: F32NarrowingFormat,
    report_key: str,
) -> DescriptorRule:
    emits = build_f32_to_narrow_float_emits(
        descriptors,
        narrow_format,
        ValueRef.operand("input"),
        ValueRef.result("result"),
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
