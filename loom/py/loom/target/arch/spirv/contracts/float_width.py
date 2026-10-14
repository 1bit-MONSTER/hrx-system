# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Exact ordinary floating-width conversions through SPIR-V integer carriers."""

from dataclasses import dataclass

from loom.dialect.scalar import conversion
from loom.dialect.vector import defs as vector
from loom.target.arch.spirv.contracts.descriptor_rule import (
    IntegerRecipe,
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
    Vector,
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


def _component(
    recipe: IntegerRecipe, element: str, value: ValueRef, lane: int
) -> ValueRef:
    if recipe.lanes == 1:
        return value
    return recipe.emit(
        f"composite_extract.v{recipe.lanes}{element}.{element}",
        {"composite": value},
        immediates={"component_index": lane},
    )


def _widening_input_bits(recipe: IntegerRecipe, source: str) -> ValueRef:
    value = ValueRef.operand("input")
    if source == "f32":
        suffix = "f32" if recipe.lanes == 1 else f"v{recipe.lanes}f32"
        return recipe.emit(
            f"bitcast.{suffix}.{recipe.integer_suffix}", {"input": value}
        )
    # Float16 does not imply Int16. Duplicating each component into a 32-bit
    # word preserves its low encoding without requesting an integer feature.
    components = []
    for lane in range(recipe.lanes):
        component = _component(recipe, "f16", value, lane)
        pair = recipe.emit(
            "composite_construct.v2f16",
            {"component0": component, "component1": component},
        )
        components.append(recipe.emit("bitcast.v2f16.i32", {"input": pair}))
    if recipe.lanes == 1:
        return components[0]
    return recipe.emit(
        f"composite_construct.{recipe.integer_suffix}",
        {f"component{lane}": value for lane, value in enumerate(components)},
    )


def _widening_result(
    recipe: IntegerRecipe, high: ValueRef, low: ValueRef | None
) -> None:
    result = ValueRef.result("result")
    if low is None:
        suffix = "f32" if recipe.lanes == 1 else f"v{recipe.lanes}f32"
        recipe.emit(
            f"bitcast.{recipe.integer_suffix}.{suffix}",
            {"input": high},
            result=result,
        )
        return
    # SPIR-V bitcasts place the first component in the least significant
    # bits. Pairing words implements Float64 without requiring Int64.
    components = []
    for lane in range(recipe.lanes):
        pair = recipe.emit(
            "composite_construct.v2i32",
            {
                "component0": _component(recipe, "i32", low, lane),
                "component1": _component(recipe, "i32", high, lane),
            },
        )
        components.append(
            recipe.emit(
                "bitcast.v2i32.f64",
                {"input": pair},
                result=result if recipe.lanes == 1 else None,
            )
        )
    if recipe.lanes != 1:
        recipe.emit(
            f"composite_construct.v{recipe.lanes}f64",
            {f"component{lane}": value for lane, value in enumerate(components)},
            result=result,
        )


def _float_width_widen_rule(
    row: _FloatWidthNarrowing, source_lanes: int
) -> DescriptorRule:
    # Reversing the narrowing matrix enumerates all native IEEE widening
    # pairs. Arithmetic stays in 32-bit scalar or ordinary-vector carriers.
    source = row.result_format
    destination = row.source_format
    recipe = IntegerRecipe(32, max(source_lanes, 1))
    bits = _widening_input_bits(recipe, row.result_type)
    constant, binary = recipe.constant, recipe.binary
    magnitude = binary("bitwise_and", bits, constant((1 << (source.bit_width - 1)) - 1))
    sign = binary("bitwise_and", bits, constant(1 << (source.bit_width - 1)))
    if source.bit_width != 32:
        sign = binary("shift_left_logical", sign, constant(32 - source.bit_width))
    subnormal = binary("s_less_than", magnitude, constant(1 << source.mantissa_bits))
    special = binary(
        "s_greater_than_equal",
        magnitude,
        constant(((1 << source.exponent_bits) - 1) << source.mantissa_bits),
    )
    zero = binary("i_equal", magnitude, constant(0))
    high_fraction_bits = destination.mantissa_bits - (destination.bit_width - 32)
    bias = recipe.select(
        special,
        constant(
            ((1 << destination.exponent_bits) - (1 << source.exponent_bits))
            << high_fraction_bits
        ),
        constant(
            (destination.exponent_bias - source.exponent_bias) << high_fraction_bits
        ),
    )
    shift = high_fraction_bits - source.mantissa_bits
    normal = binary(
        "iadd",
        binary(
            "shift_left_logical" if shift >= 0 else "shift_right_logical",
            magnitude,
            constant(abs(shift)),
        ),
        bias,
    )

    # A source subnormal contains at most 23 significant bits. Converting
    # that nonnegative integer to F32 is exact and normal (except zero).
    # Rebiasing its encoding then scales by the source subnormal unit without
    # floating arithmetic, denormal handling, or a rounding-mode requirement.
    float_suffix = "f32" if recipe.lanes == 1 else f"v{recipe.lanes}f32"
    normalized = recipe.emit(
        f"convert_s_to_f.{recipe.integer_suffix}.{float_suffix}",
        {"input": magnitude},
    )
    normalized_bits = recipe.emit(
        f"bitcast.{float_suffix}.{recipe.integer_suffix}", {"input": normalized}
    )
    normalized_high = normalized_bits
    if destination.bit_width == 64:
        normalized_high = binary("shift_right_logical", normalized_bits, constant(3))
    source_unit_exponent = 1 - source.exponent_bias - source.mantissa_bits
    subnormal_high = binary(
        "iadd",
        normalized_high,
        constant(
            (
                destination.exponent_bias
                - F32_FORMAT.exponent_bias
                + source_unit_exponent
            )
            << high_fraction_bits
        ),
    )
    high = recipe.select(subnormal, subnormal_high, normal)
    high = binary("bitwise_or", recipe.select(zero, constant(0), high), sign)
    low = None
    if destination.bit_width == 64:
        low = constant(0)
        if source.bit_width == 32:
            low = binary(
                "shift_left_logical",
                recipe.select(subnormal, normalized_bits, magnitude),
                constant(29),
            )
    _widening_result(recipe, high, low)
    input_pattern = (
        Scalar(row.result_type)
        if source_lanes == 0
        else Vector(row.result_type, lanes=source_lanes)
    )
    result_pattern = (
        Scalar(row.source_type)
        if source_lanes == 0
        else Vector(row.source_type, lanes=source_lanes)
    )
    return DescriptorRule(
        source_op=conversion.scalar_extf if source_lanes == 0 else vector.vector_extf,
        descriptor=recipe.emits[-1].descriptor,
        guards=(
            Guard.value_type("input", input_pattern),
            Guard.value_type("result", result_pattern),
            *descriptor_feature_guards(*(step.descriptor for step in recipe.emits)),
        ),
        emit=tuple(recipe.emits),
        report_key=f"exact_{row.result_name}_to_{row.source_name}",
    )


def float_width_widen_rules() -> tuple[DescriptorRule, ...]:
    """Returns portable widening for scalar and native ordinary-vector shapes."""
    return tuple(
        _float_width_widen_rule(row, source_lanes)
        for row in _FLOAT_WIDTH_NARROWINGS
        for source_lanes in (0, 1, 2, 3, 4)
    )
