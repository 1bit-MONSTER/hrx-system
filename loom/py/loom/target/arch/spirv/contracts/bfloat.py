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
    float_narrowing_descriptors,
    logical_core_descriptor,
)
from loom.target.contracts import (
    DescriptorResultType,
    DescriptorRule,
    Guard,
    Scalar,
    ValueRef,
)
from loom.target.emit.float_narrowing import (
    BF16_FORMAT,
    F32_FORMAT,
    F64_FORMAT,
    BinaryFloatFormat,
    FloatNarrowingDescriptors,
    NarrowFloatSubnormalRounding,
    build_f32_to_bf16_emits,
    build_float_to_narrow_float_emits,
)


@unique
class _BfloatNarrowResult(Enum):
    NATIVE = "native"
    I32_CARRIER = "i32_carrier"


def _bfloat_narrow_rule(
    *,
    source_type: str,
    source_name: str,
    source_format: BinaryFloatFormat,
    integer_suffix: str,
    descriptors: FloatNarrowingDescriptors,
    preserve_nan: bool,
    result_kind: _BfloatNarrowResult,
) -> DescriptorRule:
    """Rounds one binary source format to BF16 with exact RNE semantics."""
    direct_carrier_result = (
        result_kind is _BfloatNarrowResult.I32_CARRIER and integer_suffix == "i32"
    )
    carrier_ref = (
        ValueRef.result("result")
        if direct_carrier_result
        else ValueRef.temporary("carrier")
    )

    # Binary32 and BF16 have the same exponent field, so the compact bias-add
    # recipe avoids the general normal/subnormal selection.
    if source_format == F32_FORMAT:
        emits = list(
            build_f32_to_bf16_emits(
                descriptors,
                ValueRef.operand("input"),
                carrier_ref,
                preserve_nan=preserve_nan,
            )
        )
    else:
        emits = list(
            build_float_to_narrow_float_emits(
                descriptors,
                source_format,
                BF16_FORMAT,
                ValueRef.operand("input"),
                carrier_ref,
                subnormal_rounding=NarrowFloatSubnormalRounding.INTEGER,
                preserve_nan=preserve_nan,
            )
        )

    if result_kind is _BfloatNarrowResult.NATIVE:
        conversion_descriptor = logical_core_descriptor(
            f"spirv.op_s_convert.{integer_suffix}.i16"
        )
        emits.append(
            emit_descriptor_op(
                descriptor=conversion_descriptor,
                operands={"input": carrier_ref},
                results={"dst": ValueRef.temporary("narrow_bits")},
                result_types={"dst": DescriptorResultType()},
            )
        )
        bitcast_descriptor = logical_core_descriptor("spirv.op_bitcast.i16.bf16")
        emits.append(
            emit_descriptor_op(
                descriptor=bitcast_descriptor,
                operands={"input": ValueRef.temporary("narrow_bits")},
                results={"dst": ValueRef.result("result")},
            )
        )
    elif not direct_carrier_result:
        conversion_descriptor = logical_core_descriptor(
            f"spirv.op_s_convert.{integer_suffix}.i32"
        )
        emits.append(
            emit_descriptor_op(
                descriptor=conversion_descriptor,
                operands={"input": carrier_ref},
                results={"dst": ValueRef.result("result")},
            )
        )

    return DescriptorRule(
        source_op=conversion.scalar_fptrunc,
        descriptor=emits[-1].descriptor,
        guards=(
            Guard.value_type("input", Scalar(source_type)),
            Guard.value_type("result", Scalar("bf16")),
            *((Guard.value_not_nan("input"),) if not preserve_nan else ()),
            *descriptor_feature_guards(*(step.descriptor for step in emits)),
        ),
        emit=tuple(emits),
        report_key=f"exact_{source_name}_to_bfloat16",
    )


def bfloat_narrow_rules() -> tuple[DescriptorRule, ...]:
    """Returns exact F64/F32-to-BF16 rules for native and carrier profiles."""
    rows = (
        (
            "f64",
            "binary64",
            F64_FORMAT,
            "i64",
            float_narrowing_descriptors("i64", "f64"),
        ),
        (
            "f32",
            "binary32",
            F32_FORMAT,
            "i32",
            float_narrowing_descriptors("i32", "f32"),
        ),
    )
    return tuple(
        _bfloat_narrow_rule(
            source_type=source_type,
            source_name=source_name,
            source_format=source_format,
            integer_suffix=integer_suffix,
            descriptors=descriptors,
            preserve_nan=preserve_nan,
            result_kind=result_kind,
        )
        for source_type, source_name, source_format, integer_suffix, descriptors in rows
        for result_kind, preserve_nan in (
            (_BfloatNarrowResult.NATIVE, False),
            (_BfloatNarrowResult.NATIVE, True),
            (_BfloatNarrowResult.I32_CARRIER, False),
            (_BfloatNarrowResult.I32_CARRIER, True),
        )
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


def bfloat_carrier_from_i16_rule() -> DescriptorRule:
    """Constructs an i32-carried BF16 value from its native i16 bit view."""
    signed_to_unsigned = logical_core_descriptor("spirv.op_bitcast.i16.u16")
    widen = logical_core_descriptor("spirv.op_u_convert.u16.u32")
    unsigned_to_signed = logical_core_descriptor("spirv.op_bitcast.u32.i32")
    return DescriptorRule(
        source_op=conversion.scalar_bitcast,
        descriptor=unsigned_to_signed,
        guards=(
            Guard.value_type("input", Scalar("i16")),
            Guard.value_type("result", Scalar("bf16")),
            *descriptor_feature_guards(
                signed_to_unsigned,
                widen,
                unsigned_to_signed,
            ),
        ),
        emit=(
            emit_descriptor_op(
                descriptor=signed_to_unsigned,
                operands={"input": ValueRef.operand("input")},
                results={"dst": ValueRef.temporary("unsigned_bits")},
                result_types={"dst": DescriptorResultType()},
            ),
            emit_descriptor_op(
                descriptor=widen,
                operands={"input": ValueRef.temporary("unsigned_bits")},
                results={"dst": ValueRef.temporary("wide_unsigned_bits")},
                result_types={"dst": DescriptorResultType()},
            ),
            emit_descriptor_op(
                descriptor=unsigned_to_signed,
                operands={"input": ValueRef.temporary("wide_unsigned_bits")},
                results={"dst": ValueRef.result("result")},
            ),
        ),
        report_key="bfloat16_carrier_from_i16_bits",
    )
