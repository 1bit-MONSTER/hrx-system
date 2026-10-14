# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Integral rounding independent of optional Vulkan floating-point controls.

GLSL rounding is used only on finite nonzero normal inputs whose magnitude is
between one and the format's integral threshold. Integer selection handles
subnormals, signed zeros, infinities and NaNs. Ties away from zero has a separate
integer recipe: GLSL Round has an implementation-selected halfway direction.
BF16 and FP8 use that same bit-field recipe for every rounding mode because
GLSL.std.450 does not define instructions for their encodings.
"""

from dataclasses import dataclass

from loom.dialect.scalar import math as scalar_math
from loom.dialect.vector import defs as vector
from loom.target.arch.spirv.contracts.descriptor_rule import (
    IntegerRecipe,
    descriptor_feature_guards,
    logical_core_descriptor,
)
from loom.target.contracts import (
    DescriptorRule,
    EmitDescriptorOp,
    Guard,
    Scalar,
    ValueRef,
    Vector,
)


@dataclass(frozen=True, slots=True)
class _RoundingFormat:
    # Authored element type, including software-only formats.
    source_type: str
    # Total source encoding width, including the sign bit.
    bit_width: int
    # Explicit fraction field width.
    fraction_bits: int
    # Bias in the encoded exponent field.
    exponent_bias: int
    # Whether GLSL.std.450 defines this IEEE format.
    native_rounding: bool = False


_FORMATS = (
    _RoundingFormat("f16", 16, 10, 15, native_rounding=True),
    _RoundingFormat("f32", 32, 23, 127, native_rounding=True),
    _RoundingFormat("f64", 64, 52, 1023, native_rounding=True),
    _RoundingFormat("bf16", 16, 7, 127),
    _RoundingFormat("f8E4M3", 8, 3, 7),
    _RoundingFormat("f8E5M2", 8, 2, 15),
)

_SOURCE_OPS = (
    ("ceilf", scalar_math.scalar_ceilf, vector.vector_ceilf),
    ("floorf", scalar_math.scalar_floorf, vector.vector_floorf),
    ("roundf", scalar_math.scalar_roundf, vector.vector_roundf),
    ("roundevenf", scalar_math.scalar_roundevenf, vector.vector_roundevenf),
    ("truncf", scalar_math.scalar_truncf, vector.vector_truncf),
)


def _suffix(element: str, lanes: int) -> str:
    return element if lanes == 1 else f"v{lanes}{element}"


def _rounding_emits(
    format: _RoundingFormat,
    operation: str,
    integer_width: int,
    lanes: int,
    native_value: bool,
) -> tuple[EmitDescriptorOp, ...]:
    recipe = IntegerRecipe(integer_width, lanes)
    bits = ValueRef.operand("input")
    if native_value:
        bits = recipe.import_float_bits(bits, format.source_type, format.bit_width)
    binary = recipe.binary
    constant = recipe.constant
    select = recipe.select
    zero = constant(0)
    one = constant(format.exponent_bias << format.fraction_bits)
    magnitude = binary("bitwise_and", bits, constant((1 << (format.bit_width - 1)) - 1))
    sign = binary("bitwise_and", bits, constant(1 << (format.bit_width - 1)))
    tiny = binary("s_less_than", magnitude, one)
    integral = binary(
        "s_greater_than_equal",
        magnitude,
        constant((format.exponent_bias + format.fraction_bits) << format.fraction_bits),
    )

    if operation == "truncf":
        tiny_magnitude = zero
    elif operation in ("ceilf", "floorf"):
        positive = binary("i_equal", sign, zero)
        nonzero = binary("i_not_equal", magnitude, zero)
        unit = select(nonzero, one, zero)
        tiny_magnitude = (
            select(positive, unit, zero)
            if operation == "ceilf"
            else select(positive, zero, unit)
        )
    else:
        half = constant((format.exponent_bias - 1) << format.fraction_bits)
        above_half = binary(
            "s_greater_than_equal" if operation == "roundf" else "s_greater_than",
            magnitude,
            half,
        )
        tiny_magnitude = select(above_half, one, zero)

    if format.native_rounding and operation != "roundf":
        # A substituted +1 keeps both operands and results away from every
        # underspecified GLSL exceptional-value and denormal case.
        ordinary_bits = select(integral, one, select(tiny, one, bits))
        if format.bit_width != integer_width:
            packed_suffix = f"v2{format.source_type}"
            packed = recipe.emit(
                f"bitcast.i32.{packed_suffix}", {"input": ordinary_bits}
            )
            ordinary = recipe.emit(
                f"composite_extract.{packed_suffix}.{format.source_type}",
                {"composite": packed},
                immediates={"component_index": 0},
            )
        else:
            ordinary = recipe.emit(
                f"bitcast.{recipe.integer_suffix}.{_suffix(format.source_type, lanes)}",
                {"input": ordinary_bits},
            )
        rounded = recipe.emit(
            f"ext_inst.glsl_std_450.{operation}.{_suffix(format.source_type, lanes)}",
            {"input": ordinary},
        )
        if format.bit_width != integer_width:
            rounded = recipe.emit(
                f"composite_construct.v2{format.source_type}",
                {"component0": rounded, "component1": rounded},
            )
            rounded = recipe.emit(
                f"bitcast.v2{format.source_type}.i32", {"input": rounded}
            )
            rounded = binary(
                "bitwise_and", rounded, constant((1 << format.bit_width) - 1)
            )
        else:
            rounded = recipe.emit(
                f"bitcast.{_suffix(format.source_type, lanes)}.{recipe.integer_suffix}",
                {"input": rounded},
            )
    else:
        # Every shift is defined even for lanes later discarded by a select.
        # In [1, 2^fraction_bits), unit is the encoding bit for the integer LSB.
        exponent = binary(
            "shift_right_logical", magnitude, constant(format.fraction_bits)
        )
        exponent = select(tiny, constant(format.exponent_bias), exponent)
        shift = binary(
            "isub", constant(format.exponent_bias + format.fraction_bits), exponent
        )
        shift = select(integral, constant(1), shift)
        unit = binary("shift_left_logical", constant(1), shift)
        mask = binary("isub", unit, constant(1))
        if operation == "truncf":
            adjusted = magnitude
        else:
            if operation in ("ceilf", "floorf"):
                increment = (
                    select(positive, mask, zero)
                    if operation == "ceilf"
                    else select(positive, zero, mask)
                )
            else:
                increment = binary("shift_right_logical", unit, constant(1))
                if operation == "roundevenf":
                    parity = binary("shift_right_logical", magnitude, shift)
                    parity = binary("bitwise_and", parity, constant(1))
                    increment = binary(
                        "iadd", binary("isub", increment, constant(1)), parity
                    )
            adjusted = binary("iadd", magnitude, increment)
        keep_mask = binary("bitwise_xor", mask, constant(-1))
        rounded = binary("bitwise_and", adjusted, keep_mask)

    rounded = select(tiny, tiny_magnitude, rounded)
    rounded = binary("bitwise_or", rounded, sign)
    # Preserve the input encoding for infinities, NaNs and already integral
    # values. A packed native half may carry duplicate high bits, harmless to
    # the final extract; software carriers already have zero high bits.
    result = select(
        integral,
        bits,
        rounded,
        result=None if native_value else ValueRef.result("result"),
    )
    if native_value:
        recipe.export_float_bits(result, format.source_type, format.bit_width)
    return tuple(recipe.emits)


def rounding_rules() -> tuple[DescriptorRule, ...]:
    """Returns complete rounding rules in native-before-carrier order."""
    rules: list[DescriptorRule] = []
    for format in _FORMATS:
        # Ordinary vectors use matching integer lanes. The scalar packed path
        # also covers devices with Float16/BFloat16 but without Int16. Software
        # BF16/FP8 vectors use the established scalar packetization boundary.
        representations = []
        if format.source_type in ("f16", "f32", "f64", "bf16"):
            representations.extend(
                (format.bit_width, lanes, True) for lanes in (0, 1, 2, 3, 4)
            )
            if format.bit_width == 16:
                representations.extend((32, lanes, True) for lanes in (0, 1))
        if not format.native_rounding:
            representations.append(
                (32 if format.source_type == "bf16" else 8, 0, False)
            )
        for operation, scalar_op, vector_op in _SOURCE_OPS:
            for integer_width, source_lanes, native_value in representations:
                lanes = max(1, source_lanes)
                pattern = (
                    Scalar(format.source_type)
                    if source_lanes == 0
                    else Vector(format.source_type, lanes=source_lanes)
                )
                # Ignoring NaNs, infinities and zero signs makes truncation and
                # ties-even invariant under GLSL denormal flushing. Directed
                # rounding is not: ceil(+subnormal) must still produce +1.
                if (
                    format.native_rounding
                    and integer_width == format.bit_width
                    and operation in ("truncf", "roundevenf")
                ):
                    descriptor = logical_core_descriptor(
                        f"spirv.op_ext_inst.glsl_std_450.{operation}."
                        f"{_suffix(format.source_type, lanes)}"
                    )
                    rules.append(
                        DescriptorRule(
                            source_op=scalar_op if source_lanes == 0 else vector_op,
                            descriptor=descriptor,
                            guards=(
                                Guard.value_type("input", pattern),
                                Guard.value_type("result", pattern),
                                *(
                                    Guard.instance_flags_has_all("fastmath", flag)
                                    for flag in ("nnan", "ninf", "nsz")
                                ),
                                *descriptor_feature_guards(descriptor),
                            ),
                            emit=(
                                EmitDescriptorOp(
                                    descriptor=descriptor,
                                    operands={"input": ValueRef.operand("input")},
                                    results={"dst": ValueRef.result("result")},
                                ),
                            ),
                        )
                    )
                emits = _rounding_emits(
                    format, operation, integer_width, lanes, native_value
                )
                rules.append(
                    DescriptorRule(
                        source_op=scalar_op if source_lanes == 0 else vector_op,
                        descriptor=emits[-1].descriptor,
                        guards=(
                            Guard.value_type("input", pattern),
                            Guard.value_type("result", pattern),
                            *descriptor_feature_guards(
                                *(step.descriptor for step in emits)
                            ),
                        ),
                        emit=emits,
                        report_key=(
                            "guarded_native_rounding"
                            if format.native_rounding and operation != "roundf"
                            else "integer_rounding"
                        ),
                    )
                )
    return tuple(rules)
