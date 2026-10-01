# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Target-neutral exact F32-to-narrow-float descriptor recipes."""

from __future__ import annotations

from dataclasses import dataclass
from enum import Enum, unique

from loom.target.contracts import (
    DescriptorEmitForm,
    DescriptorResultType,
    EmitDescriptorOp,
    SourceValueKind,
    ValueRef,
)
from loom.target.low_descriptors import Descriptor


@unique
class NarrowFloatOverflow(Enum):
    """Destination encoding used when a finite F32 value overflows."""

    INFINITY = "infinity"
    SATURATE = "saturate"


@unique
class NarrowFloatNan(Enum):
    """Destination encoding used for an F32 NaN."""

    PRESERVE_PAYLOAD = "preserve_payload"
    CANONICAL = "canonical"


@dataclass(frozen=True, slots=True)
class F32NarrowingFormat:
    """IEEE-like destination format and its exceptional-value policies."""

    exponent_bits: int
    mantissa_bits: int
    exponent_bias: int
    overflow: NarrowFloatOverflow
    nan: NarrowFloatNan


F16_FORMAT = F32NarrowingFormat(
    exponent_bits=5,
    mantissa_bits=10,
    exponent_bias=15,
    overflow=NarrowFloatOverflow.INFINITY,
    nan=NarrowFloatNan.PRESERVE_PAYLOAD,
)

F8E4M3_FORMAT = F32NarrowingFormat(
    exponent_bits=4,
    mantissa_bits=3,
    exponent_bias=7,
    overflow=NarrowFloatOverflow.SATURATE,
    nan=NarrowFloatNan.CANONICAL,
)

F8E5M2_FORMAT = F32NarrowingFormat(
    exponent_bits=5,
    mantissa_bits=2,
    exponent_bias=15,
    overflow=NarrowFloatOverflow.INFINITY,
    nan=NarrowFloatNan.CANONICAL,
)


@dataclass(frozen=True, slots=True)
class F32NarrowingDescriptors:
    """Concrete target descriptors required by exact narrowing recipes."""

    i32_constant: Descriptor
    f32_constant: Descriptor
    i32_add: Descriptor
    i32_subtract: Descriptor
    i32_shift_right_logical: Descriptor
    i32_bitwise_and: Descriptor
    i32_bitwise_or: Descriptor
    i32_less_than_nonnegative: Descriptor
    i32_greater_than_equal_nonnegative: Descriptor
    i32_greater_than_nonnegative: Descriptor
    f32_add: Descriptor
    reinterpret_f32_as_i32: Descriptor
    reinterpret_i32_as_f32: Descriptor
    i32_select: Descriptor


class _ScalarRecipe:
    """Builds one compact straight-line scalar descriptor recipe."""

    def __init__(self, descriptors: F32NarrowingDescriptors) -> None:
        self.descriptors = descriptors
        self.emits: list[EmitDescriptorOp] = []

    def _operation(
        self,
        result: ValueRef,
        descriptor: Descriptor,
        operands: dict[str, ValueRef],
    ) -> ValueRef:
        result_types = None
        if result.kind is SourceValueKind.TEMPORARY:
            result_types = {"dst": DescriptorResultType()}
        self.emits.append(
            EmitDescriptorOp(
                descriptor=descriptor,
                operands=operands,
                results={"dst": result},
                result_types=result_types,
            )
        )
        return result

    @staticmethod
    def _constant_immediate(descriptor: Descriptor, value: int) -> dict[str, int]:
        (immediate,) = descriptor.immediates
        modulus = 1 << immediate.bit_width
        encoded_value = value & (modulus - 1)
        if encoded_value > immediate.unsigned_max:
            encoded_value -= modulus
        return {immediate.field_name: encoded_value}

    def i32_constant(self, result_name: str, value: int) -> ValueRef:
        result = ValueRef.temporary(result_name)
        descriptor = self.descriptors.i32_constant
        self.emits.append(
            EmitDescriptorOp(
                descriptor=descriptor,
                results={"dst": result},
                result_types={"dst": DescriptorResultType()},
                immediates=self._constant_immediate(descriptor, value),
                form=DescriptorEmitForm.CONST,
            )
        )
        return result

    def f32_constant(self, result_name: str, bits: int) -> ValueRef:
        result = ValueRef.temporary(result_name)
        descriptor = self.descriptors.f32_constant
        self.emits.append(
            EmitDescriptorOp(
                descriptor=descriptor,
                results={"dst": result},
                result_types={"dst": DescriptorResultType()},
                immediates=self._constant_immediate(descriptor, bits),
                form=DescriptorEmitForm.CONST,
            )
        )
        return result

    def i32_binary(
        self,
        result_name: str,
        descriptor: Descriptor,
        lhs: ValueRef,
        rhs: ValueRef,
    ) -> ValueRef:
        return self._operation(
            ValueRef.temporary(result_name),
            descriptor,
            {"lhs": lhs, "rhs": rhs},
        )

    def i32_binary_to(
        self,
        result: ValueRef,
        descriptor: Descriptor,
        lhs: ValueRef,
        rhs: ValueRef,
    ) -> ValueRef:
        return self._operation(result, descriptor, {"lhs": lhs, "rhs": rhs})

    def f32_add(self, result_name: str, lhs: ValueRef, rhs: ValueRef) -> ValueRef:
        return self._operation(
            ValueRef.temporary(result_name),
            self.descriptors.f32_add,
            {"lhs": lhs, "rhs": rhs},
        )

    def reinterpret_f32_as_i32(self, result_name: str, value: ValueRef) -> ValueRef:
        return self._operation(
            ValueRef.temporary(result_name),
            self.descriptors.reinterpret_f32_as_i32,
            {"input": value},
        )

    def reinterpret_i32_as_f32(self, result_name: str, value: ValueRef) -> ValueRef:
        return self._operation(
            ValueRef.temporary(result_name),
            self.descriptors.reinterpret_i32_as_f32,
            {"input": value},
        )

    def i32_select(
        self,
        result_name: str,
        true_value: ValueRef,
        false_value: ValueRef,
        condition: ValueRef,
    ) -> ValueRef:
        return self._operation(
            ValueRef.temporary(result_name),
            self.descriptors.i32_select,
            {
                "true_value": true_value,
                "false_value": false_value,
                "condition": condition,
            },
        )

    def i32_select_to(
        self,
        result: ValueRef,
        true_value: ValueRef,
        false_value: ValueRef,
        condition: ValueRef,
    ) -> ValueRef:
        return self._operation(
            result,
            self.descriptors.i32_select,
            {
                "true_value": true_value,
                "false_value": false_value,
                "condition": condition,
            },
        )


def build_f32_to_bf16_emits(
    descriptors: F32NarrowingDescriptors,
    input_ref: ValueRef,
    result_ref: ValueRef,
) -> tuple[EmitDescriptorOp, ...]:
    """Rounds F32 to BF16 while preserving every source NaN as a NaN."""

    recipe = _ScalarRecipe(descriptors)
    input_bits = recipe.reinterpret_f32_as_i32("input_bits", input_ref)
    shift = recipe.i32_constant("shift", 16)
    upper = recipe.i32_binary(
        "upper", descriptors.i32_shift_right_logical, input_bits, shift
    )
    one = recipe.i32_constant("one", 1)
    retained_lsb = recipe.i32_binary(
        "retained_lsb", descriptors.i32_bitwise_and, upper, one
    )
    rounding_bias = recipe.i32_constant("rounding_bias", 0x7FFF)
    bias = recipe.i32_binary("bias", descriptors.i32_add, rounding_bias, retained_lsb)
    rounded = recipe.i32_binary("rounded", descriptors.i32_add, input_bits, bias)
    finite = recipe.i32_binary(
        "finite", descriptors.i32_shift_right_logical, rounded, shift
    )

    nonsign_mask = recipe.i32_constant("nonsign_mask", 0x7FFFFFFF)
    magnitude = recipe.i32_binary(
        "magnitude", descriptors.i32_bitwise_and, input_bits, nonsign_mask
    )
    infinity_bits = recipe.i32_constant("infinity_bits", 0x7F800000)
    is_nan = recipe.i32_binary(
        "is_nan", descriptors.i32_greater_than_nonnegative, magnitude, infinity_bits
    )
    quiet_nan_bit = recipe.i32_constant("quiet_nan_bit", 0x0040)
    nan = recipe.i32_binary("nan", descriptors.i32_bitwise_or, upper, quiet_nan_bit)
    recipe.i32_select_to(result_ref, nan, finite, is_nan)
    return tuple(recipe.emits)


def build_f32_to_narrow_float_emits(
    descriptors: F32NarrowingDescriptors,
    narrow_format: F32NarrowingFormat,
    input_ref: ValueRef,
    result_ref: ValueRef,
) -> tuple[EmitDescriptorOp, ...]:
    """Rounds F32 exactly using target F32 addition and integer carriers."""

    recipe = _ScalarRecipe(descriptors)
    input_bits = recipe.reinterpret_f32_as_i32("input_bits", input_ref)

    sign_mask = recipe.i32_constant("sign_mask", 0x80000000)
    sign = recipe.i32_binary("sign", descriptors.i32_bitwise_and, input_bits, sign_mask)
    sign_shift = recipe.i32_constant(
        "sign_shift",
        32 - (1 + narrow_format.exponent_bits + narrow_format.mantissa_bits),
    )
    sign = recipe.i32_binary(
        "positioned_sign", descriptors.i32_shift_right_logical, sign, sign_shift
    )

    nonsign_mask = recipe.i32_constant("nonsign_mask", 0x7FFFFFFF)
    magnitude = recipe.i32_binary(
        "magnitude_bits", descriptors.i32_bitwise_and, input_bits, nonsign_mask
    )

    # The ULP of this power of two equals the destination minimum subnormal.
    # F32 addition performs the complete subnormal ties-to-even operation;
    # subtracting the magic encoding exposes the narrow carrier payload.
    magic_exponent = (
        127 - narrow_format.exponent_bias + (23 - narrow_format.mantissa_bits) + 1
    )
    magic_bits_value = magic_exponent << 23
    magic = recipe.f32_constant("subnormal_magic", magic_bits_value)
    magnitude_float = recipe.reinterpret_i32_as_f32("magnitude", magnitude)
    biased_subnormal = recipe.f32_add("biased_subnormal", magnitude_float, magic)
    biased_subnormal_bits = recipe.reinterpret_f32_as_i32(
        "biased_subnormal_bits", biased_subnormal
    )
    magic_bits = recipe.i32_constant("subnormal_magic_bits", magic_bits_value)
    subnormal = recipe.i32_binary(
        "subnormal", descriptors.i32_subtract, biased_subnormal_bits, magic_bits
    )

    normal_shift_value = 23 - narrow_format.mantissa_bits
    normal_shift = recipe.i32_constant("normal_shift", normal_shift_value)
    normal_truncated = recipe.i32_binary(
        "normal_truncated",
        descriptors.i32_shift_right_logical,
        magnitude,
        normal_shift,
    )
    one = recipe.i32_constant("one", 1)
    retained_lsb = recipe.i32_binary(
        "normal_retained_lsb", descriptors.i32_bitwise_and, normal_truncated, one
    )
    rounding_bias = (1 << (normal_shift_value - 1)) - 1
    exponent_rebias = (narrow_format.exponent_bias - 127) << 23
    normal_bias = recipe.i32_constant("normal_bias", exponent_rebias + rounding_bias)
    normal_rounded = recipe.i32_binary(
        "normal_biased", descriptors.i32_add, magnitude, normal_bias
    )
    normal_rounded = recipe.i32_binary(
        "normal_rounded", descriptors.i32_add, normal_rounded, retained_lsb
    )
    normal = recipe.i32_binary(
        "normal", descriptors.i32_shift_right_logical, normal_rounded, normal_shift
    )

    minimum_normal_bits = recipe.i32_constant(
        "minimum_normal_bits", (127 - narrow_format.exponent_bias + 1) << 23
    )
    is_subnormal = recipe.i32_binary(
        "is_subnormal",
        descriptors.i32_less_than_nonnegative,
        magnitude,
        minimum_normal_bits,
    )
    finite = recipe.i32_select("finite_unclamped", subnormal, normal, is_subnormal)

    special_payload_value = (
        (1 << narrow_format.exponent_bits) - 1
    ) << narrow_format.mantissa_bits
    nan_payload_value = special_payload_value | ((1 << narrow_format.mantissa_bits) - 1)
    special_payload = recipe.i32_constant("special_payload", special_payload_value)
    if narrow_format.overflow is NarrowFloatOverflow.INFINITY:
        needs_clamp = recipe.i32_binary(
            "needs_clamp",
            descriptors.i32_greater_than_equal_nonnegative,
            finite,
            special_payload,
        )
        finite = recipe.i32_select("finite", special_payload, finite, needs_clamp)
    else:
        nan_payload = recipe.i32_constant("nan_payload", nan_payload_value)
        maximum_finite = recipe.i32_constant("maximum_finite", nan_payload_value - 1)
        needs_clamp = recipe.i32_binary(
            "needs_clamp",
            descriptors.i32_greater_than_equal_nonnegative,
            finite,
            nan_payload,
        )
        finite = recipe.i32_select("finite", maximum_finite, finite, needs_clamp)

    if narrow_format.nan is NarrowFloatNan.PRESERVE_PAYLOAD:
        fraction_mask = recipe.i32_constant("fraction_mask", 0x007FFFFF)
        fraction = recipe.i32_binary(
            "fraction", descriptors.i32_bitwise_and, magnitude, fraction_mask
        )
        nan_payload = recipe.i32_binary(
            "source_nan_payload",
            descriptors.i32_shift_right_logical,
            fraction,
            normal_shift,
        )
        nan = recipe.i32_binary(
            "nan_payload", descriptors.i32_bitwise_or, special_payload, nan_payload
        )
        quiet_nan_bit = recipe.i32_constant(
            "quiet_nan_bit", 1 << (narrow_format.mantissa_bits - 1)
        )
        nan = recipe.i32_binary("nan", descriptors.i32_bitwise_or, nan, quiet_nan_bit)
    else:
        nan = recipe.i32_constant("nan", nan_payload_value)

    infinity_bits = recipe.i32_constant("source_infinity_bits", 0x7F800000)
    is_nan = recipe.i32_binary(
        "is_nan", descriptors.i32_greater_than_nonnegative, magnitude, infinity_bits
    )
    unsigned_result = recipe.i32_select("unsigned_result", nan, finite, is_nan)
    recipe.i32_binary_to(result_ref, descriptors.i32_bitwise_or, sign, unsigned_result)
    return tuple(recipe.emits)
