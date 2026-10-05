# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Exact scalar float-narrowing rules shared by x86 SIMD profiles."""

from collections.abc import Callable
from dataclasses import replace

from loom.dialect.scalar import conversion as scalar_conversion
from loom.target.contracts import (
    DescriptorResultType,
    DescriptorRule,
    EmitDescriptorOp,
    Guard,
    Scalar,
    TypePattern,
    ValueRef,
)
from loom.target.emit.float_narrowing import (
    BF16_SOURCE_FORMAT,
    F8E4M3_FORMAT,
    F8E5M2_FORMAT,
    F16_FORMAT,
    F16_SOURCE_FORMAT,
    F32_FORMAT,
    BinaryFloatFormat,
    IntegerNarrowingDescriptors,
    IntegerNarrowingImmediateForms,
    NarrowFloatFormat,
    NarrowFloatSubnormalRounding,
    build_f32_bits_to_bf16_emits,
    build_integer_bits_to_narrow_float_emits,
)
from loom.target.low_descriptors import Descriptor

_DescriptorLookup = Callable[[str], Descriptor]

_F8E4M3 = Scalar("f8E4M3")
_F8E5M2 = Scalar("f8E5M2")
_F16 = Scalar("f16")
_BF16 = Scalar("bf16")
_F32 = Scalar("f32")


def _integer_narrowing_descriptors(
    descriptor_lookup: _DescriptorLookup,
) -> IntegerNarrowingDescriptors:
    return IntegerNarrowingDescriptors(
        integer_bit_width=32,
        integer_constant=descriptor_lookup("x86.scalar.movimm.gpr32"),
        integer_add=descriptor_lookup("x86.scalar.add.gpr32"),
        integer_subtract=descriptor_lookup("x86.scalar.sub.gpr32"),
        integer_shift_left=descriptor_lookup("x86.scalar.shl.cl.gpr32"),
        integer_shift_right_logical=descriptor_lookup("x86.scalar.shr.cl.gpr32"),
        integer_bitwise_and=descriptor_lookup("x86.scalar.and.gpr32"),
        integer_bitwise_or=descriptor_lookup("x86.scalar.or.gpr32"),
        integer_less_than_nonnegative=descriptor_lookup("x86.scalar.cmp.slt.gpr32"),
        integer_greater_than_equal_nonnegative=descriptor_lookup(
            "x86.scalar.cmp.sge.gpr32"
        ),
        integer_greater_than_nonnegative=descriptor_lookup("x86.scalar.cmp.sgt.gpr32"),
        integer_select=descriptor_lookup("x86.scalar.select.gpr32"),
        immediate_forms=IntegerNarrowingImmediateForms(
            add=descriptor_lookup("x86.scalar.add.imm.gpr32"),
            subtract=descriptor_lookup("x86.scalar.sub.imm.gpr32"),
            shift_left=descriptor_lookup("x86.scalar.shl.imm.gpr32"),
            shift_right_logical=descriptor_lookup("x86.scalar.shr.imm.gpr32"),
            bitwise_and=descriptor_lookup("x86.scalar.and.imm.gpr32"),
            bitwise_or=descriptor_lookup("x86.scalar.or.imm.gpr32"),
            less_than_nonnegative=descriptor_lookup("x86.scalar.cmp.slt.imm.gpr32"),
            greater_than_equal_nonnegative=descriptor_lookup(
                "x86.scalar.cmp.sge.imm.gpr32"
            ),
            greater_than_nonnegative=descriptor_lookup("x86.scalar.cmp.sgt.imm.gpr32"),
        ),
    )


def _f32_bits_emit(
    descriptor_lookup: _DescriptorLookup,
) -> EmitDescriptorOp:
    return EmitDescriptorOp(
        descriptor=descriptor_lookup("x86.avx2.vmovd.gpr32.xmm"),
        operands={"input": ValueRef.operand("input")},
        results={"dst": ValueRef.temporary("input_bits")},
        result_types={"dst": DescriptorResultType()},
    )


def _rule(
    source_type: TypePattern,
    result_type: TypePattern,
    emits: tuple[EmitDescriptorOp, ...],
    report_key: str,
) -> DescriptorRule:
    # Variable x86 shifts consume their count through ECX. Bind that physical
    # register only at the shift use so the computed count remains an ordinary
    # GPR value for its other uses.
    emits = tuple(
        replace(emit, copy_operands=("rhs",))
        if emit.descriptor.key in ("x86.scalar.shl.cl.gpr32", "x86.scalar.shr.cl.gpr32")
        else emit
        for emit in emits
    )
    return DescriptorRule(
        source_op=scalar_conversion.scalar_fptrunc,
        descriptor=emits[-1].descriptor,
        guards=(
            Guard.value_type("input", source_type),
            Guard.value_type("result", result_type),
        ),
        emit=emits,
        report_key=report_key,
    )


def _f32_to_bf16_rule(
    descriptor_lookup: _DescriptorLookup,
    descriptors: IntegerNarrowingDescriptors,
) -> DescriptorRule:
    emits = (
        _f32_bits_emit(descriptor_lookup),
        *build_f32_bits_to_bf16_emits(
            descriptors,
            ValueRef.temporary("input_bits"),
            ValueRef.result("result"),
        ),
    )
    return _rule(_F32, _BF16, emits, "exact_binary32_to_bfloat16")


def _f32_to_narrow_rule(
    descriptor_lookup: _DescriptorLookup,
    descriptors: IntegerNarrowingDescriptors,
    result_type: TypePattern,
    result_format: NarrowFloatFormat,
    result_name: str,
) -> DescriptorRule:
    emits = (
        _f32_bits_emit(descriptor_lookup),
        *build_integer_bits_to_narrow_float_emits(
            descriptors,
            F32_FORMAT,
            result_format,
            ValueRef.temporary("input_bits"),
            ValueRef.result("result"),
            subnormal_rounding=NarrowFloatSubnormalRounding.INTEGER,
        ),
    )
    return _rule(_F32, result_type, emits, f"exact_binary32_to_{result_name}")


def _integer_carrier_to_float8_rule(
    descriptors: IntegerNarrowingDescriptors,
    source_type: TypePattern,
    source_format: BinaryFloatFormat,
    source_name: str,
    result_type: TypePattern,
    result_format: NarrowFloatFormat,
    result_name: str,
) -> DescriptorRule:
    if (
        source_format.exponent_bias
        >= result_format.exponent_bias + result_format.mantissa_bits + 1
    ):
        subnormal_rounding = NarrowFloatSubnormalRounding.INTEGER
    else:
        subnormal_rounding = NarrowFloatSubnormalRounding.INTEGER_SOURCE_SUBNORMALS
    emits = build_integer_bits_to_narrow_float_emits(
        descriptors,
        source_format,
        result_format,
        ValueRef.operand("input"),
        ValueRef.result("result"),
        subnormal_rounding=subnormal_rounding,
    )
    return _rule(
        source_type,
        result_type,
        emits,
        f"exact_{source_name}_to_{result_name}",
    )


def x86_float_narrowing_rules(
    descriptor_lookup: _DescriptorLookup,
) -> tuple[DescriptorRule, ...]:
    """Returns the exact narrow-float matrix supported by x86 carriers."""

    descriptors = _integer_narrowing_descriptors(descriptor_lookup)
    return (
        _f32_to_bf16_rule(descriptor_lookup, descriptors),
        _f32_to_narrow_rule(
            descriptor_lookup,
            descriptors,
            _F16,
            F16_FORMAT,
            "binary16",
        ),
        _f32_to_narrow_rule(
            descriptor_lookup,
            descriptors,
            _F8E4M3,
            F8E4M3_FORMAT,
            "float8_e4m3",
        ),
        _f32_to_narrow_rule(
            descriptor_lookup,
            descriptors,
            _F8E5M2,
            F8E5M2_FORMAT,
            "float8_e5m2",
        ),
        *(
            _integer_carrier_to_float8_rule(
                descriptors,
                source_type,
                source_format,
                source_name,
                result_type,
                result_format,
                result_name,
            )
            for source_type, source_format, source_name in (
                (_F16, F16_SOURCE_FORMAT, "binary16"),
                (_BF16, BF16_SOURCE_FORMAT, "bfloat16"),
            )
            for result_type, result_format, result_name in (
                (_F8E4M3, F8E4M3_FORMAT, "float8_e4m3"),
                (_F8E5M2, F8E5M2_FORMAT, "float8_e5m2"),
            )
        ),
    )
