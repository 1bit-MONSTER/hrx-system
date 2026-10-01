# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Exact F32-to-narrow-float Wasm contract recipes."""

from __future__ import annotations

from collections.abc import Callable
from enum import Enum

from loom.dialect.scalar import conversion as scalar_conversion
from loom.target.contracts import (
    DescriptorEmitForm,
    DescriptorRule,
    EmitDescriptorOp,
    Guard,
    Scalar,
    TypePattern,
    ValueRef,
)
from loom.target.low_descriptors import Descriptor

_I32 = Scalar("i32")
_F8E4M3 = Scalar("f8E4M3")
_F8E5M2 = Scalar("f8E5M2")
_F16 = Scalar("f16")
_BF16 = Scalar("bf16")
_F32 = Scalar("f32")


class _OverflowEncoding(Enum):
    INFINITY = "infinity"
    SATURATE = "saturate"


class _NanEncoding(Enum):
    PRESERVE_PAYLOAD = "preserve_payload"
    CANONICAL = "canonical"


class _ScalarRecipe:
    """Builds one compact straight-line Wasm scalar contract recipe."""

    def __init__(self, descriptor_lookup: Callable[[str], Descriptor]) -> None:
        self._descriptor_lookup = descriptor_lookup
        self.emits: list[EmitDescriptorOp] = []

    def _operation(
        self,
        result_name: str | None,
        descriptor_key: str,
        result_type: TypePattern,
        operands: dict[str, ValueRef],
    ) -> ValueRef:
        result = (
            ValueRef.result("result")
            if result_name is None
            else ValueRef.temporary(result_name)
        )
        self.emits.append(
            EmitDescriptorOp(
                descriptor=self._descriptor_lookup(descriptor_key),
                operands=operands,
                results={"dst": result},
                result_types=(None if result_name is None else {"dst": result_type}),
            )
        )
        return result

    def i32_constant(self, result_name: str, value: int) -> ValueRef:
        result = ValueRef.temporary(result_name)
        self.emits.append(
            EmitDescriptorOp(
                descriptor=self._descriptor_lookup("wasm.i32.const"),
                results={"dst": result},
                result_types={"dst": _I32},
                immediates={"i32_value": value & 0xFFFFFFFF},
                form=DescriptorEmitForm.CONST,
            )
        )
        return result

    def f32_constant(self, result_name: str, bits: int) -> ValueRef:
        result = ValueRef.temporary(result_name)
        self.emits.append(
            EmitDescriptorOp(
                descriptor=self._descriptor_lookup("wasm.f32.const"),
                results={"dst": result},
                result_types={"dst": _F32},
                immediates={"bits": bits},
                form=DescriptorEmitForm.CONST,
            )
        )
        return result

    def i32_binary(
        self,
        result_name: str | None,
        operation: str,
        lhs: ValueRef,
        rhs: ValueRef,
    ) -> ValueRef:
        return self._operation(
            result_name,
            f"wasm.i32.{operation}",
            _I32,
            {"lhs": lhs, "rhs": rhs},
        )

    def f32_binary(
        self,
        result_name: str,
        operation: str,
        lhs: ValueRef,
        rhs: ValueRef,
    ) -> ValueRef:
        return self._operation(
            result_name,
            f"wasm.f32.{operation}",
            _F32,
            {"lhs": lhs, "rhs": rhs},
        )

    def reinterpret_f32_as_i32(self, result_name: str, value: ValueRef) -> ValueRef:
        return self._operation(
            result_name,
            "wasm.i32.reinterpret_f32",
            _I32,
            {"input": value},
        )

    def reinterpret_i32_as_f32(self, result_name: str, value: ValueRef) -> ValueRef:
        return self._operation(
            result_name,
            "wasm.f32.reinterpret_i32",
            _F32,
            {"input": value},
        )

    def i32_select(
        self,
        result_name: str | None,
        true_value: ValueRef,
        false_value: ValueRef,
        condition: ValueRef,
    ) -> ValueRef:
        return self._operation(
            result_name,
            "wasm.i32.select",
            _I32,
            {
                "true_value": true_value,
                "false_value": false_value,
                "condition": condition,
            },
        )


def _f32_to_bf16_rule(
    descriptor_lookup: Callable[[str], Descriptor],
    type_guard: Callable[[str, TypePattern], Guard],
) -> DescriptorRule:
    """Rounds F32 to BF16 while preserving every source NaN as a NaN."""

    recipe = _ScalarRecipe(descriptor_lookup)
    input_bits = recipe.reinterpret_f32_as_i32("input_bits", ValueRef.operand("input"))
    shift = recipe.i32_constant("shift", 16)
    upper = recipe.i32_binary("upper", "shr_u", input_bits, shift)
    one = recipe.i32_constant("one", 1)
    retained_lsb = recipe.i32_binary("retained_lsb", "and", upper, one)
    rounding_bias = recipe.i32_constant("rounding_bias", 0x7FFF)
    bias = recipe.i32_binary("bias", "add", rounding_bias, retained_lsb)
    rounded = recipe.i32_binary("rounded", "add", input_bits, bias)
    finite = recipe.i32_binary("finite", "shr_u", rounded, shift)

    nonsign_mask = recipe.i32_constant("nonsign_mask", 0x7FFFFFFF)
    magnitude = recipe.i32_binary("magnitude", "and", input_bits, nonsign_mask)
    infinity_bits = recipe.i32_constant("infinity_bits", 0x7F800000)
    is_nan = recipe.i32_binary("is_nan", "gt_u", magnitude, infinity_bits)
    quiet_nan_bit = recipe.i32_constant("quiet_nan_bit", 0x0040)
    nan = recipe.i32_binary("nan", "or", upper, quiet_nan_bit)
    recipe.i32_select(None, nan, finite, is_nan)

    return DescriptorRule(
        source_op=scalar_conversion.scalar_fptrunc,
        descriptor=recipe.emits[-1].descriptor,
        guards=(
            type_guard("input", _F32),
            type_guard("result", _BF16),
        ),
        emit=tuple(recipe.emits),
        report_key="exact_binary32_to_bfloat16",
    )


def _f32_to_narrow_float_rule(
    descriptor_lookup: Callable[[str], Descriptor],
    type_guard: Callable[[str, TypePattern], Guard],
    result_type: TypePattern,
    *,
    exponent_bits: int,
    mantissa_bits: int,
    exponent_bias: int,
    overflow_encoding: _OverflowEncoding,
    nan_encoding: _NanEncoding,
    report_key: str,
) -> DescriptorRule:
    """Rounds F32 exactly using Wasm F32 addition and integer carriers."""

    recipe = _ScalarRecipe(descriptor_lookup)
    input_bits = recipe.reinterpret_f32_as_i32("input_bits", ValueRef.operand("input"))

    sign_mask = recipe.i32_constant("sign_mask", 0x80000000)
    sign = recipe.i32_binary("sign", "and", input_bits, sign_mask)
    sign_shift = recipe.i32_constant(
        "sign_shift", 32 - (1 + exponent_bits + mantissa_bits)
    )
    sign = recipe.i32_binary("positioned_sign", "shr_u", sign, sign_shift)

    nonsign_mask = recipe.i32_constant("nonsign_mask", 0x7FFFFFFF)
    magnitude = recipe.i32_binary("magnitude_bits", "and", input_bits, nonsign_mask)

    # The ULP of this power of two equals the destination minimum subnormal.
    # F32 addition performs the complete subnormal ties-to-even operation;
    # subtracting the magic encoding exposes the narrow carrier payload.
    magic_exponent = 127 - exponent_bias + (23 - mantissa_bits) + 1
    magic_bits_value = magic_exponent << 23
    magic = recipe.f32_constant("subnormal_magic", magic_bits_value)
    magnitude_float = recipe.reinterpret_i32_as_f32("magnitude", magnitude)
    biased_subnormal = recipe.f32_binary(
        "biased_subnormal", "add", magnitude_float, magic
    )
    biased_subnormal_bits = recipe.reinterpret_f32_as_i32(
        "biased_subnormal_bits", biased_subnormal
    )
    magic_bits = recipe.i32_constant("subnormal_magic_bits", magic_bits_value)
    subnormal = recipe.i32_binary("subnormal", "sub", biased_subnormal_bits, magic_bits)

    normal_shift_value = 23 - mantissa_bits
    normal_shift = recipe.i32_constant("normal_shift", normal_shift_value)
    normal_truncated = recipe.i32_binary(
        "normal_truncated", "shr_u", magnitude, normal_shift
    )
    one = recipe.i32_constant("one", 1)
    retained_lsb = recipe.i32_binary(
        "normal_retained_lsb", "and", normal_truncated, one
    )
    rounding_bias = (1 << (normal_shift_value - 1)) - 1
    exponent_rebias = (exponent_bias - 127) << 23
    normal_bias = recipe.i32_constant("normal_bias", exponent_rebias + rounding_bias)
    normal_rounded = recipe.i32_binary("normal_biased", "add", magnitude, normal_bias)
    normal_rounded = recipe.i32_binary(
        "normal_rounded", "add", normal_rounded, retained_lsb
    )
    normal = recipe.i32_binary("normal", "shr_u", normal_rounded, normal_shift)

    minimum_normal_bits = recipe.i32_constant(
        "minimum_normal_bits", (127 - exponent_bias + 1) << 23
    )
    is_subnormal = recipe.i32_binary(
        "is_subnormal", "lt_u", magnitude, minimum_normal_bits
    )
    finite = recipe.i32_select("finite_unclamped", subnormal, normal, is_subnormal)

    special_payload_value = ((1 << exponent_bits) - 1) << mantissa_bits
    nan_payload_value = special_payload_value | ((1 << mantissa_bits) - 1)
    special_payload = recipe.i32_constant("special_payload", special_payload_value)
    if overflow_encoding is _OverflowEncoding.INFINITY:
        needs_clamp = recipe.i32_binary("needs_clamp", "ge_u", finite, special_payload)
        finite = recipe.i32_select("finite", special_payload, finite, needs_clamp)
    else:
        nan_payload = recipe.i32_constant("nan_payload", nan_payload_value)
        maximum_finite = recipe.i32_constant("maximum_finite", nan_payload_value - 1)
        needs_clamp = recipe.i32_binary("needs_clamp", "ge_u", finite, nan_payload)
        finite = recipe.i32_select("finite", maximum_finite, finite, needs_clamp)

    if nan_encoding is _NanEncoding.PRESERVE_PAYLOAD:
        fraction_mask = recipe.i32_constant("fraction_mask", 0x007FFFFF)
        fraction = recipe.i32_binary("fraction", "and", magnitude, fraction_mask)
        nan_payload = recipe.i32_binary(
            "source_nan_payload", "shr_u", fraction, normal_shift
        )
        nan = recipe.i32_binary("nan_payload", "or", special_payload, nan_payload)
        quiet_nan_bit = recipe.i32_constant("quiet_nan_bit", 1 << (mantissa_bits - 1))
        nan = recipe.i32_binary("nan", "or", nan, quiet_nan_bit)
    else:
        nan = recipe.i32_constant("nan", nan_payload_value)

    infinity_bits = recipe.i32_constant("source_infinity_bits", 0x7F800000)
    is_nan = recipe.i32_binary("is_nan", "gt_u", magnitude, infinity_bits)
    result = recipe.i32_select("unsigned_result", nan, finite, is_nan)
    recipe.i32_binary(None, "or", sign, result)

    return DescriptorRule(
        source_op=scalar_conversion.scalar_fptrunc,
        descriptor=recipe.emits[-1].descriptor,
        guards=(
            type_guard("input", _F32),
            type_guard("result", result_type),
        ),
        emit=tuple(recipe.emits),
        report_key=report_key,
    )


def f32_narrowing_rules(
    descriptor_lookup: Callable[[str], Descriptor],
    type_guard: Callable[[str, TypePattern], Guard],
) -> tuple[DescriptorRule, ...]:
    """Returns exact F32-to-narrow-float rules for Wasm scalar carriers."""

    return (
        _f32_to_bf16_rule(descriptor_lookup, type_guard),
        _f32_to_narrow_float_rule(
            descriptor_lookup,
            type_guard,
            _F16,
            exponent_bits=5,
            mantissa_bits=10,
            exponent_bias=15,
            overflow_encoding=_OverflowEncoding.INFINITY,
            nan_encoding=_NanEncoding.PRESERVE_PAYLOAD,
            report_key="exact_binary32_to_f16",
        ),
        _f32_to_narrow_float_rule(
            descriptor_lookup,
            type_guard,
            _F8E4M3,
            exponent_bits=4,
            mantissa_bits=3,
            exponent_bias=7,
            overflow_encoding=_OverflowEncoding.SATURATE,
            nan_encoding=_NanEncoding.CANONICAL,
            report_key="exact_binary32_to_f8e4m3",
        ),
        _f32_to_narrow_float_rule(
            descriptor_lookup,
            type_guard,
            _F8E5M2,
            exponent_bits=5,
            mantissa_bits=2,
            exponent_bias=15,
            overflow_encoding=_OverflowEncoding.INFINITY,
            nan_encoding=_NanEncoding.CANONICAL,
            report_key="exact_binary32_to_f8e5m2",
        ),
    )
