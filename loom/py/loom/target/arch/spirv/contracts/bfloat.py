# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""BF16 conversions with source-defined rounding independent of Vulkan defaults."""

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


def bfloat_narrow_rule() -> DescriptorRule:
    """Rounds binary32 to BF16, preserving infinities and quieting NaNs."""
    # Vulkan permits implementation-defined rounding for BF16 OpFConvert.
    # Integer rounding also supports ordinary SSA consumers: FPRoundingMode
    # decorations constrain the converted value's uses to storage/conversions.
    emit: list[EmitDescriptorOp] = []
    for name, value in (
        ("shift", 16),
        ("one", 1),
        ("rounding_bias", 0x7FFF),
        ("magnitude_mask", 0x7FFFFFFF),
        ("infinity", 0x7F800000),
        ("quiet_bit", 0x40),
    ):
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
    for operation, result, lhs, rhs in (
        ("shift_right_logical", "upper", "bits", "shift"),
        ("bitwise_and", "least_bit", "upper", "one"),
        ("iadd", "bias", "least_bit", "rounding_bias"),
        ("iadd", "rounded", "bits", "bias"),
        ("shift_right_logical", "rounded_upper", "rounded", "shift"),
        ("bitwise_and", "magnitude", "bits", "magnitude_mask"),
        ("s_greater_than", "is_nan", "magnitude", "infinity"),
        ("bitwise_or", "quiet_nan", "upper", "quiet_bit"),
    ):
        emit.append(
            emit_descriptor_op(
                descriptor=logical_core_descriptor(f"spirv.op_{operation}.i32"),
                operands={
                    "lhs": ValueRef.temporary(lhs),
                    "rhs": ValueRef.temporary(rhs),
                },
                results={"dst": ValueRef.temporary(result)},
                result_types={"dst": DescriptorResultType()},
            )
        )
    emit.extend(
        (
            emit_descriptor_op(
                descriptor=logical_core_descriptor("spirv.op_select.i32"),
                operands={
                    "condition": ValueRef.temporary("is_nan"),
                    "true_value": ValueRef.temporary("quiet_nan"),
                    "false_value": ValueRef.temporary("rounded_upper"),
                },
                results={"dst": ValueRef.temporary("selected")},
                result_types={"dst": DescriptorResultType()},
            ),
            emit_descriptor_op(
                descriptor=logical_core_descriptor("spirv.op_s_convert.i32.i16"),
                operands={"input": ValueRef.temporary("selected")},
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
            *descriptor_feature_guards(*(step.descriptor for step in emit)),
        ),
        emit=tuple(emit),
    )
