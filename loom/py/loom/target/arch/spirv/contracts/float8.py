# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Exact F32-to-FP8 conversion rules using SPIR-V integer carriers."""

from loom.dialect.scalar import conversion
from loom.target.arch.spirv.contracts.descriptor_rule import (
    descriptor_feature_guards,
    emit_descriptor_op,
    logical_core_descriptor,
)
from loom.target.contracts import DescriptorRule, Guard, Scalar, ValueRef
from loom.target.emit.f32_narrowing import (
    F8E4M3_FORMAT,
    F8E5M2_FORMAT,
    F32NarrowingDescriptors,
    F32NarrowingFormat,
    build_f32_to_narrow_float_emits,
)

_F32 = Scalar("f32")


def _narrowing_descriptors() -> F32NarrowingDescriptors:
    # Every comparison operand is nonnegative, so signed i32 comparisons avoid
    # introducing u32 bitcasts while preserving the common recipe's ordering.
    return F32NarrowingDescriptors(
        i32_constant=logical_core_descriptor("spirv.op_constant.i32"),
        f32_constant=logical_core_descriptor("spirv.op_constant.f32"),
        i32_add=logical_core_descriptor("spirv.op_iadd.i32"),
        i32_subtract=logical_core_descriptor("spirv.op_isub.i32"),
        i32_shift_right_logical=logical_core_descriptor(
            "spirv.op_shift_right_logical.i32"
        ),
        i32_bitwise_and=logical_core_descriptor("spirv.op_bitwise_and.i32"),
        i32_bitwise_or=logical_core_descriptor("spirv.op_bitwise_or.i32"),
        i32_less_than_nonnegative=logical_core_descriptor("spirv.op_s_less_than.i32"),
        i32_greater_than_equal_nonnegative=logical_core_descriptor(
            "spirv.op_s_greater_than_equal.i32"
        ),
        i32_greater_than_nonnegative=logical_core_descriptor(
            "spirv.op_s_greater_than.i32"
        ),
        f32_add=logical_core_descriptor("spirv.op_fadd.f32"),
        reinterpret_f32_as_i32=logical_core_descriptor("spirv.op_bitcast.f32.i32"),
        reinterpret_i32_as_f32=logical_core_descriptor("spirv.op_bitcast.i32.f32"),
        i32_select=logical_core_descriptor("spirv.op_select.i32"),
    )


def _float8_narrow_rule(
    descriptors: F32NarrowingDescriptors,
    result_type: Scalar,
    narrow_format: F32NarrowingFormat,
    report_key: str,
) -> DescriptorRule:
    carrier = ValueRef.temporary("carrier")
    emits = list(
        build_f32_to_narrow_float_emits(
            descriptors,
            narrow_format,
            ValueRef.operand("input"),
            carrier,
        )
    )
    conversion_descriptor = logical_core_descriptor("spirv.op_s_convert.i32.i8")
    emits.append(
        emit_descriptor_op(
            descriptor=conversion_descriptor,
            operands={"input": carrier},
            results={"dst": ValueRef.result("result")},
        )
    )
    return DescriptorRule(
        source_op=conversion.scalar_fptrunc,
        descriptor=conversion_descriptor,
        guards=(
            Guard.value_type("input", _F32),
            Guard.value_type("result", result_type),
            *descriptor_feature_guards(conversion_descriptor),
        ),
        emit=tuple(emits),
        report_key=report_key,
    )


def float8_narrow_rules() -> tuple[DescriptorRule, ...]:
    """Returns exact F32-to-FP8 rules ending in native i8 carriers."""

    descriptors = _narrowing_descriptors()
    return (
        _float8_narrow_rule(
            descriptors,
            Scalar("f8E4M3"),
            F8E4M3_FORMAT,
            "exact_binary32_to_f8e4m3",
        ),
        _float8_narrow_rule(
            descriptors,
            Scalar("f8E5M2"),
            F8E5M2_FORMAT,
            "exact_binary32_to_f8e5m2",
        ),
    )
