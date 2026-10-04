# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Exact scalar-to-FP8 conversion rules using SPIR-V integer carriers."""

from loom.dialect.scalar import conversion
from loom.target.arch.spirv.contracts.descriptor_rule import (
    descriptor_feature_guards,
    emit_descriptor_op,
    float_narrowing_descriptors,
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
from loom.target.emit.float_narrowing import (
    F8E4M3_FORMAT,
    F8E5M2_FORMAT,
    F32_FORMAT,
    F64_FORMAT,
    BinaryFloatFormat,
    FloatNarrowingDescriptors,
    NarrowFloatFormat,
    NarrowFloatSubnormalRounding,
    build_float_to_narrow_float_emits,
)

_F8E4M3 = Scalar("f8E4M3")
_F8E5M2 = Scalar("f8E5M2")


def _float8_narrow_rule(
    *,
    source_type: Scalar,
    source_name: str,
    source_format: BinaryFloatFormat,
    descriptors: FloatNarrowingDescriptors,
    integer_suffix: str,
    input_ref: ValueRef,
    prefix_emits: tuple[EmitDescriptorOp, ...],
    result_type: Scalar,
    result_name: str,
    narrow_format: NarrowFloatFormat,
) -> DescriptorRule:
    carrier = ValueRef.temporary("carrier")
    emits = list(prefix_emits)
    emits.extend(
        build_float_to_narrow_float_emits(
            descriptors,
            source_format,
            narrow_format,
            input_ref,
            carrier,
            subnormal_rounding=NarrowFloatSubnormalRounding.INTEGER,
        )
    )
    conversion_descriptor = logical_core_descriptor(
        f"spirv.op_s_convert.{integer_suffix}.i8"
    )
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
            Guard.value_type("input", source_type),
            Guard.value_type("result", result_type),
            *descriptor_feature_guards(*(step.descriptor for step in emits)),
        ),
        emit=tuple(emits),
        report_key=f"exact_{source_name}_to_{result_name}",
    )


def _float8_narrow_rules_for_source(
    *,
    source_type: Scalar,
    source_name: str,
    source_format: BinaryFloatFormat,
    descriptors: FloatNarrowingDescriptors,
    integer_suffix: str,
    input_ref: ValueRef,
    prefix_emits: tuple[EmitDescriptorOp, ...] = (),
) -> tuple[DescriptorRule, ...]:
    return tuple(
        _float8_narrow_rule(
            source_type=source_type,
            source_name=source_name,
            source_format=source_format,
            descriptors=descriptors,
            integer_suffix=integer_suffix,
            input_ref=input_ref,
            prefix_emits=prefix_emits,
            result_type=result_type,
            result_name=result_name,
            narrow_format=narrow_format,
        )
        for result_type, result_name, narrow_format in (
            (_F8E4M3, "f8e4m3", F8E4M3_FORMAT),
            (_F8E5M2, "f8e5m2", F8E5M2_FORMAT),
        )
    )


def _widen_to_f32_emit(source_suffix: str) -> EmitDescriptorOp:
    descriptor = logical_core_descriptor(f"spirv.op_f_convert.{source_suffix}.f32")
    return emit_descriptor_op(
        descriptor=descriptor,
        operands={"input": ValueRef.operand("input")},
        results={"dst": ValueRef.temporary("wide_input")},
        result_types={"dst": DescriptorResultType()},
    )


def _bfloat_carrier_to_f32_emits() -> tuple[EmitDescriptorOp, ...]:
    constant_descriptor = logical_core_descriptor("spirv.op_constant.i32")
    left_shift_descriptor = logical_core_descriptor("spirv.op_shift_left_logical.i32")
    bitcast_descriptor = logical_core_descriptor("spirv.op_bitcast.i32.f32")
    return (
        EmitDescriptorOp(
            descriptor=constant_descriptor,
            results={"dst": ValueRef.temporary("carrier_shift")},
            result_types={"dst": DescriptorResultType()},
            immediates={"i32_value": 16},
            form=DescriptorEmitForm.CONST,
        ),
        emit_descriptor_op(
            descriptor=left_shift_descriptor,
            operands={
                "lhs": ValueRef.operand("input"),
                "rhs": ValueRef.temporary("carrier_shift"),
            },
            results={"dst": ValueRef.temporary("wide_bits")},
            result_types={"dst": DescriptorResultType()},
        ),
        emit_descriptor_op(
            descriptor=bitcast_descriptor,
            operands={"input": ValueRef.temporary("wide_bits")},
            results={"dst": ValueRef.temporary("wide_input")},
            result_types={"dst": DescriptorResultType()},
        ),
    )


def float8_narrow_rules() -> tuple[DescriptorRule, ...]:
    """Returns the complete scalar narrowing matrix ending in i8 carriers."""

    f32_descriptors = float_narrowing_descriptors("i32", "f32")
    rules: list[DescriptorRule] = []
    rules.extend(
        _float8_narrow_rules_for_source(
            source_type=Scalar("f64"),
            source_name="binary64",
            source_format=F64_FORMAT,
            descriptors=float_narrowing_descriptors("i64", "f64"),
            integer_suffix="i64",
            input_ref=ValueRef.operand("input"),
        )
    )
    rules.extend(
        _float8_narrow_rules_for_source(
            source_type=Scalar("f32"),
            source_name="binary32",
            source_format=F32_FORMAT,
            descriptors=f32_descriptors,
            integer_suffix="i32",
            input_ref=ValueRef.operand("input"),
        )
    )
    rules.extend(
        _float8_narrow_rules_for_source(
            source_type=Scalar("f16"),
            source_name="binary16",
            source_format=F32_FORMAT,
            descriptors=f32_descriptors,
            integer_suffix="i32",
            input_ref=ValueRef.temporary("wide_input"),
            prefix_emits=(_widen_to_f32_emit("f16"),),
        )
    )
    rules.extend(
        _float8_narrow_rules_for_source(
            source_type=Scalar("bf16"),
            source_name="bfloat16",
            source_format=F32_FORMAT,
            descriptors=f32_descriptors,
            integer_suffix="i32",
            input_ref=ValueRef.temporary("wide_input"),
            prefix_emits=(_widen_to_f32_emit("bf16"),),
        )
    )
    rules.extend(
        _float8_narrow_rules_for_source(
            source_type=Scalar("bf16"),
            source_name="bfloat16",
            source_format=F32_FORMAT,
            descriptors=f32_descriptors,
            integer_suffix="i32",
            input_ref=ValueRef.temporary("wide_input"),
            prefix_emits=_bfloat_carrier_to_f32_emits(),
        )
    )
    return tuple(rules)
