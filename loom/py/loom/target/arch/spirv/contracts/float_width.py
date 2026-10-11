# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Exact ordinary floating-width narrowing through SPIR-V integer carriers."""

from dataclasses import dataclass

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
    F16_FORMAT,
    F32_FORMAT,
    F32_NARROW_FORMAT,
    F64_FORMAT,
    BinaryFloatFormat,
    NarrowFloatFormat,
    NarrowFloatSubnormalRounding,
    build_float_to_narrow_float_emits,
)


@dataclass(frozen=True, slots=True)
class _FloatWidthNarrowing:
    source_type: str
    source_name: str
    source_format: BinaryFloatFormat
    integer_suffix: str
    result_type: str
    result_name: str
    result_format: NarrowFloatFormat
    packed_result_suffix: str


_FLOAT_WIDTH_NARROWINGS = (
    _FloatWidthNarrowing(
        source_type="f64",
        source_name="binary64",
        source_format=F64_FORMAT,
        integer_suffix="i64",
        result_type="f32",
        result_name="binary32",
        result_format=F32_NARROW_FORMAT,
        packed_result_suffix="v2f32",
    ),
    _FloatWidthNarrowing(
        source_type="f64",
        source_name="binary64",
        source_format=F64_FORMAT,
        integer_suffix="i64",
        result_type="f16",
        result_name="binary16",
        result_format=F16_FORMAT,
        packed_result_suffix="v4f16",
    ),
    _FloatWidthNarrowing(
        source_type="f32",
        source_name="binary32",
        source_format=F32_FORMAT,
        integer_suffix="i32",
        result_type="f16",
        result_name="binary16",
        result_format=F16_FORMAT,
        packed_result_suffix="v2f16",
    ),
)


def _float_width_narrow_rule(row: _FloatWidthNarrowing) -> DescriptorRule:
    descriptors = float_narrowing_descriptors(row.integer_suffix, row.source_type)
    emits = list(
        build_float_to_narrow_float_emits(
            descriptors,
            row.source_format,
            row.result_format,
            ValueRef.operand("input"),
            ValueRef.temporary("carrier"),
            subnormal_rounding=NarrowFloatSubnormalRounding.INTEGER,
        )
    )
    bitcast_descriptor = logical_core_descriptor(
        f"spirv.op_bitcast.{row.integer_suffix}.{row.packed_result_suffix}"
    )
    emits.append(
        emit_descriptor_op(
            descriptor=bitcast_descriptor,
            operands={"input": ValueRef.temporary("carrier")},
            results={"dst": ValueRef.temporary("packed_result")},
            result_types={"dst": DescriptorResultType()},
        )
    )
    extract_descriptor = logical_core_descriptor(
        f"spirv.op_composite_extract.{row.packed_result_suffix}.{row.result_type}"
    )
    emits.append(
        emit_descriptor_op(
            descriptor=extract_descriptor,
            operands={"composite": ValueRef.temporary("packed_result")},
            results={"dst": ValueRef.result("result")},
            immediates={"component_index": 0},
        )
    )
    return DescriptorRule(
        source_op=conversion.scalar_fptrunc,
        descriptor=extract_descriptor,
        guards=(
            Guard.value_type("input", Scalar(row.source_type)),
            Guard.value_type("result", Scalar(row.result_type)),
            *descriptor_feature_guards(*(step.descriptor for step in emits)),
        ),
        emit=tuple(emits),
        report_key=f"exact_{row.source_name}_to_{row.result_name}",
    )


def float_width_narrow_rules() -> tuple[DescriptorRule, ...]:
    """Returns exact fallback rules for the native floating-width matrix."""
    return tuple(_float_width_narrow_rule(row) for row in _FLOAT_WIDTH_NARROWINGS)
