# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""BF16 conversions with source-defined rounding independent of Vulkan defaults."""

from enum import Enum, unique

from loom.dialect.scalar import conversion
from loom.target.arch.spirv.contracts.descriptor_rule import (
    descriptor_feature_guards,
    emit_descriptor_op,
    logical_core_descriptor,
)
from loom.target.contracts import (
    DescriptorEmitForm,
    DescriptorResultType,
    DescriptorRule,
    EmitDescriptorOp,
    Guard,
    Scalar,
    ValueRef,
)


@unique
class _BfloatNarrowResult(Enum):
    NATIVE = "native"
    I32_CARRIER = "i32_carrier"


def _bfloat_narrow_rule(
    *, preserve_nan: bool, result_kind: _BfloatNarrowResult
) -> DescriptorRule:
    """Rounds binary32 to BF16, preserving infinities and quieting NaNs."""
    # Vulkan permits implementation-defined rounding for BF16 OpFConvert.
    # Integer rounding also supports ordinary SSA consumers: FPRoundingMode
    # decorations constrain the converted value's uses to storage/conversions.
    emit: list[EmitDescriptorOp] = []
    constants = (
        ("shift", 16),
        ("one", 1),
        ("rounding_bias", 0x7FFF),
    )
    if preserve_nan:
        constants += (
            ("magnitude_mask", 0x7FFFFFFF),
            ("infinity", 0x7F800000),
            ("quiet_bit", 0x40),
        )
    for name, value in constants:
        emit.append(
            EmitDescriptorOp(
                descriptor=logical_core_descriptor("spirv.op_constant.i32"),
                results={"dst": ValueRef.temporary(name)},
                result_types={"dst": Scalar("i32")},
                immediates={"i32_value": value},
                form=DescriptorEmitForm.CONST,
            )
        )
    emit.append(
        emit_descriptor_op(
            descriptor=logical_core_descriptor("spirv.op_bitcast.f32.i32"),
            operands={"input": ValueRef.operand("input")},
            results={"dst": ValueRef.temporary("bits")},
            result_types={"dst": DescriptorResultType()},
        )
    )
    # Adding 0x7fff + the retained LSB rounds halfway cases to even. Masking
    # the sign before the comparison makes the signed NaN test nonnegative.
    operations = (
        ("shift_right_logical", "upper", "bits", "shift"),
        ("bitwise_and", "least_bit", "upper", "one"),
        ("iadd", "bias", "least_bit", "rounding_bias"),
        ("iadd", "rounded", "bits", "bias"),
        ("shift_right_logical", "rounded_upper", "rounded", "shift"),
    )
    if preserve_nan:
        operations += (
            ("bitwise_and", "magnitude", "bits", "magnitude_mask"),
            ("s_greater_than", "is_nan", "magnitude", "infinity"),
            ("bitwise_or", "quiet_nan", "upper", "quiet_bit"),
        )
    carrier_result = "selected" if preserve_nan else "rounded_upper"
    for operation, result, lhs, rhs in operations:
        binds_carrier_result = (
            result_kind is _BfloatNarrowResult.I32_CARRIER and result == carrier_result
        )
        result_ref = (
            ValueRef.result("result")
            if binds_carrier_result
            else ValueRef.temporary(result)
        )
        emit.append(
            emit_descriptor_op(
                descriptor=logical_core_descriptor(f"spirv.op_{operation}.i32"),
                operands={
                    "lhs": ValueRef.temporary(lhs),
                    "rhs": ValueRef.temporary(rhs),
                },
                results={"dst": result_ref},
                result_types=(
                    None if binds_carrier_result else {"dst": DescriptorResultType()}
                ),
            )
        )
    if preserve_nan:
        result_ref = (
            ValueRef.result("result")
            if result_kind is _BfloatNarrowResult.I32_CARRIER
            else ValueRef.temporary("selected")
        )
        emit.append(
            emit_descriptor_op(
                descriptor=logical_core_descriptor("spirv.op_select.i32"),
                operands={
                    "condition": ValueRef.temporary("is_nan"),
                    "true_value": ValueRef.temporary("quiet_nan"),
                    "false_value": ValueRef.temporary("rounded_upper"),
                },
                results={"dst": result_ref},
                result_types=(
                    {"dst": DescriptorResultType()}
                    if result_kind is _BfloatNarrowResult.NATIVE
                    else None
                ),
            )
        )
    if result_kind is _BfloatNarrowResult.NATIVE:
        emit.extend(
            (
                emit_descriptor_op(
                    descriptor=logical_core_descriptor("spirv.op_s_convert.i32.i16"),
                    operands={
                        "input": ValueRef.temporary(
                            "selected" if preserve_nan else "rounded_upper"
                        )
                    },
                    results={"dst": ValueRef.temporary("narrow_bits")},
                    result_types={"dst": DescriptorResultType()},
                ),
                emit_descriptor_op(
                    descriptor=logical_core_descriptor("spirv.op_bitcast.i16.bf16"),
                    operands={"input": ValueRef.temporary("narrow_bits")},
                    results={"dst": ValueRef.result("result")},
                ),
            )
        )
    return DescriptorRule(
        source_op=conversion.scalar_fptrunc,
        descriptor=emit[-1].descriptor,
        guards=(
            Guard.value_type("input", Scalar("f32")),
            Guard.value_type("result", Scalar("bf16")),
            *((Guard.value_not_nan("input"),) if not preserve_nan else ()),
            *descriptor_feature_guards(*(step.descriptor for step in emit)),
        ),
        emit=tuple(emit),
    )


def bfloat_narrow_native_rule(*, preserve_nan: bool) -> DescriptorRule:
    """Rounds binary32 to a native SPIR-V BF16 value."""
    return _bfloat_narrow_rule(
        preserve_nan=preserve_nan,
        result_kind=_BfloatNarrowResult.NATIVE,
    )


def bfloat_narrow_carrier_rule(*, preserve_nan: bool) -> DescriptorRule:
    """Rounds binary32 to BF16 bits held in an i32 carrier."""
    return _bfloat_narrow_rule(
        preserve_nan=preserve_nan,
        result_kind=_BfloatNarrowResult.I32_CARRIER,
    )


def bfloat_carrier_to_i16_rule() -> DescriptorRule:
    """Observes an i32-carried BF16 value through its native i16 bit view."""
    descriptor = logical_core_descriptor("spirv.op_s_convert.i32.i16")
    return DescriptorRule(
        source_op=conversion.scalar_bitcast,
        descriptor=descriptor,
        guards=(
            Guard.value_type("input", Scalar("bf16")),
            Guard.value_type("result", Scalar("i16")),
            *descriptor_feature_guards(descriptor),
        ),
        emit=(
            emit_descriptor_op(
                descriptor=descriptor,
                operands={"input": ValueRef.operand("input")},
                results={"dst": ValueRef.result("result")},
            ),
        ),
    )
