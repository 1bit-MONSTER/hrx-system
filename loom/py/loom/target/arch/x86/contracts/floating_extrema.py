# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""x86 IEEE floating-point extrema composition."""

from __future__ import annotations

from loom.dialect.scalar import arithmetic as scalar_arithmetic
from loom.dialect.vector import defs as vector
from loom.target.arch.x86.contracts.rule_builders import (
    DescriptorLookup as _DescriptorLookup,
)
from loom.target.arch.x86.contracts.rule_builders import (
    emit_descriptor_op as _op_emit,
)
from loom.target.arch.x86.vector_families import (
    AVX2_FLOAT_COMPARE_MNEMONICS,
    AVX2_FLOAT_VECTOR_BIT_WIDTHS,
    AVX2_SCALAR_FLOAT_EXTREMA_MNEMONICS,
    AVX512_FLOAT_COMPARE_MNEMONICS,
    AVX512_FP16_FLOAT_COMPARE_MNEMONIC,
    AVX512_FP16_FLOAT_EXTREMA_MNEMONICS,
    AVX512_FP16_SCALAR_FLOAT_COMPARE_MNEMONIC,
    AVX512_FP16_SCALAR_FLOAT_EXTREMA_MNEMONICS,
    AVX512_FP16_VECTOR_BIT_WIDTHS,
    AVX512_SELECT_MNEMONICS,
    FLOAT_ELEMENTS,
    FLOAT_EXTREMA_MNEMONICS,
    FLOAT_EXTREMA_OPERATIONS,
    FP16_ELEMENT,
    VectorElement,
)
from loom.target.contracts import (
    DescriptorResultType,
    DescriptorRule,
    EmitDescriptorOp,
    Guard,
    Scalar,
    TypePattern,
    ValueRef,
    Vector,
)
from loom.target.low_descriptors import Descriptor

_REGISTER_SUFFIXES = {
    32: "xmm",
    64: "xmm",
    128: "xmm",
    256: "ymm",
    512: "zmm",
}
_FLOAT_COMPARE_IMMEDIATES = {
    "oeq": 0,
    "ogt": 30,
    "olt": 17,
    "uno": 3,
}
_SCALAR_SOURCE_OPS = {
    "minimumf": scalar_arithmetic.scalar_minimumf,
    "maximumf": scalar_arithmetic.scalar_maximumf,
    "minnumf": scalar_arithmetic.scalar_minnumf,
    "maxnumf": scalar_arithmetic.scalar_maxnumf,
}
_VECTOR_SOURCE_OPS = {
    "minimumf": vector.vector_minimumf,
    "maximumf": vector.vector_maximumf,
    "minnumf": vector.vector_minnumf,
    "maxnumf": vector.vector_maxnumf,
}


def _float_extrema_emit_chain(
    operation: str,
    lhs: ValueRef,
    rhs: ValueRef,
    result: ValueRef,
    compare: Descriptor,
    tie: Descriptor,
    select: Descriptor,
    *,
    temporary_prefix: str = "",
) -> tuple[EmitDescriptorOp, ...]:
    """Emits exact IEEE extrema semantics using resolved ISA operations."""
    is_minimum = operation in ("minimumf", "minnumf")
    propagates_nan = operation in ("minimumf", "maximumf")

    def temporary(name: str) -> ValueRef:
        return ValueRef.temporary(f"{temporary_prefix}{name}")

    ordered = temporary("ordered")
    candidate = temporary("candidate")
    equal = temporary("equal")
    tie_value = temporary("tie_value")
    finite_result = temporary("finite_result")
    lhs_nan = temporary("lhs_nan")
    lhs_result = temporary("lhs_result")
    rhs_nan = temporary("rhs_nan")
    descriptor_result_type = {"dst": DescriptorResultType()}
    return (
        _op_emit(
            descriptor=compare,
            operands={"lhs": lhs, "rhs": rhs},
            results={"dst": ordered},
            result_types=descriptor_result_type,
            immediates={
                "predicate": _FLOAT_COMPARE_IMMEDIATES["olt" if is_minimum else "ogt"]
            },
        ),
        _op_emit(
            descriptor=select,
            operands={"false_value": rhs, "true_value": lhs, "mask": ordered},
            results={"dst": candidate},
            result_types=descriptor_result_type,
        ),
        _op_emit(
            descriptor=compare,
            operands={"lhs": lhs, "rhs": rhs},
            results={"dst": equal},
            result_types=descriptor_result_type,
            immediates={"predicate": _FLOAT_COMPARE_IMMEDIATES["oeq"]},
        ),
        _op_emit(
            descriptor=tie,
            operands={"lhs": lhs, "rhs": rhs},
            results={"dst": tie_value},
            result_types=descriptor_result_type,
        ),
        _op_emit(
            descriptor=select,
            operands={
                "false_value": candidate,
                "true_value": tie_value,
                "mask": equal,
            },
            results={"dst": finite_result},
            result_types=descriptor_result_type,
        ),
        _op_emit(
            descriptor=compare,
            operands={"lhs": lhs, "rhs": lhs},
            results={"dst": lhs_nan},
            result_types=descriptor_result_type,
            immediates={"predicate": _FLOAT_COMPARE_IMMEDIATES["uno"]},
        ),
        _op_emit(
            descriptor=select,
            operands={
                "false_value": finite_result,
                "true_value": lhs if propagates_nan else rhs,
                "mask": lhs_nan,
            },
            results={"dst": lhs_result},
            result_types=descriptor_result_type,
        ),
        _op_emit(
            descriptor=compare,
            operands={"lhs": rhs, "rhs": rhs},
            results={"dst": rhs_nan},
            result_types=descriptor_result_type,
            immediates={"predicate": _FLOAT_COMPARE_IMMEDIATES["uno"]},
        ),
        _op_emit(
            descriptor=select,
            operands={
                "false_value": lhs_result,
                "true_value": rhs if propagates_nan else lhs,
                "mask": rhs_nan,
            },
            results={"dst": result},
        ),
    )


def _avx2_float_extrema_descriptors(
    operation: str,
    element: VectorElement,
    register_bit_width: int,
    descriptor_lookup: _DescriptorLookup,
) -> tuple[Descriptor, Descriptor, Descriptor]:
    register_suffix = _REGISTER_SUFFIXES[register_bit_width]
    return (
        descriptor_lookup(
            f"x86.avx2.{AVX2_FLOAT_COMPARE_MNEMONICS[element.name]}.{register_suffix}"
        ),
        descriptor_lookup(
            f"x86.avx2."
            f"{'vpor' if operation in ('minimumf', 'minnumf') else 'vpand'}."
            f"{register_suffix}"
        ),
        descriptor_lookup(f"x86.avx2.vpblendvb.{register_suffix}"),
    )


def _avx512_float_extrema_descriptors(
    operation: str,
    element: VectorElement,
    descriptor_lookup: _DescriptorLookup,
) -> tuple[Descriptor, Descriptor, Descriptor]:
    return (
        descriptor_lookup(
            f"x86.avx512.{AVX512_FLOAT_COMPARE_MNEMONICS[element.name]}.zmm"
        ),
        descriptor_lookup(
            "x86.avx512."
            f"{'vpord' if operation in ('minimumf', 'minnumf') else 'vpandd'}."
            "zmm"
        ),
        descriptor_lookup(f"x86.avx512.{AVX512_SELECT_MNEMONICS[element.name]}.zmm"),
    )


def _avx512_fp16_float_extrema_descriptors(
    operation: str,
    vector_bit_width: int,
    descriptor_lookup: _DescriptorLookup,
    *,
    scalar: bool = False,
) -> tuple[Descriptor, Descriptor, Descriptor]:
    register_suffix = _REGISTER_SUFFIXES[vector_bit_width]
    is_minimum = operation in ("minimumf", "minnumf")
    if vector_bit_width == 512:
        tie_key = f"x86.avx512.{'vpord' if is_minimum else 'vpandd'}.zmm"
    else:
        tie_key = f"x86.avx2.{'vpor' if is_minimum else 'vpand'}.{register_suffix}"
    compare_mnemonic = (
        AVX512_FP16_SCALAR_FLOAT_COMPARE_MNEMONIC
        if scalar
        else AVX512_FP16_FLOAT_COMPARE_MNEMONIC
    )
    return (
        descriptor_lookup(f"x86.avx512_fp16.{compare_mnemonic}.{register_suffix}"),
        descriptor_lookup(tie_key),
        descriptor_lookup(f"x86.avx512.vpblendmw.{register_suffix}"),
    )


def _float_extrema_rule(
    operation: str,
    source_type: TypePattern,
    source_op,
    descriptors: tuple[Descriptor, Descriptor, Descriptor],
    *,
    priority: int = 0,
) -> DescriptorRule:
    compare, tie, select = descriptors
    emits = _float_extrema_emit_chain(
        operation,
        ValueRef.operand("lhs"),
        ValueRef.operand("rhs"),
        ValueRef.result("result"),
        compare,
        tie,
        select,
    )
    return DescriptorRule(
        source_op=source_op,
        descriptor=select,
        guards=(
            Guard.value_type("lhs", source_type),
            Guard.value_type("rhs", source_type),
            Guard.value_type("result", source_type),
            *(Guard.descriptor_available(descriptor) for descriptor in (compare, tie)),
        ),
        emit=emits,
        priority=priority,
    )


def _fast_float_extrema_rule(
    source_type: TypePattern,
    descriptor: Descriptor,
    source_op,
    *,
    priority: int = 1,
) -> DescriptorRule:
    return DescriptorRule(
        source_op=source_op,
        descriptor=descriptor,
        guards=(
            Guard.instance_flags_has_all("fastmath", "nnan"),
            Guard.instance_flags_has_all("fastmath", "nsz"),
            Guard.value_type("lhs", source_type),
            Guard.value_type("rhs", source_type),
            Guard.value_type("result", source_type),
        ),
        emit=(
            _op_emit(
                descriptor=descriptor,
                operands={
                    "lhs": ValueRef.operand("lhs"),
                    "rhs": ValueRef.operand("rhs"),
                },
                results={"dst": ValueRef.result("result")},
            ),
        ),
        priority=priority,
    )


def avx2_float_extrema_rules(
    descriptor_lookup: _DescriptorLookup,
) -> tuple[DescriptorRule, ...]:
    """Generates scalar and packed f32/f64 IEEE extrema families."""
    fast_scalar_rules = (
        _fast_float_extrema_rule(
            Scalar(element.name),
            descriptor_lookup(
                f"x86.avx2."
                f"{AVX2_SCALAR_FLOAT_EXTREMA_MNEMONICS[operation][element.name]}."
                "xmm"
            ),
            _SCALAR_SOURCE_OPS[operation],
        )
        for operation in FLOAT_EXTREMA_OPERATIONS
        for element in FLOAT_ELEMENTS
    )
    exact_scalar_rules = (
        _float_extrema_rule(
            operation,
            Scalar(element.name),
            _SCALAR_SOURCE_OPS[operation],
            _avx2_float_extrema_descriptors(operation, element, 128, descriptor_lookup),
        )
        for operation in FLOAT_EXTREMA_OPERATIONS
        for element in FLOAT_ELEMENTS
    )
    fast_vector_rules = (
        _fast_float_extrema_rule(
            Vector(element.name, lanes=vector_bit_width // element.bit_width),
            descriptor_lookup(
                f"x86.avx2."
                f"{FLOAT_EXTREMA_MNEMONICS[operation][element.name]}."
                f"{_REGISTER_SUFFIXES[vector_bit_width]}"
            ),
            _VECTOR_SOURCE_OPS[operation],
        )
        for operation in FLOAT_EXTREMA_OPERATIONS
        for element in FLOAT_ELEMENTS
        for vector_bit_width in AVX2_FLOAT_VECTOR_BIT_WIDTHS
        if vector_bit_width >= element.bit_width
    )
    exact_vector_rules = (
        _float_extrema_rule(
            operation,
            Vector(element.name, lanes=vector_bit_width // element.bit_width),
            _VECTOR_SOURCE_OPS[operation],
            _avx2_float_extrema_descriptors(
                operation, element, vector_bit_width, descriptor_lookup
            ),
        )
        for operation in FLOAT_EXTREMA_OPERATIONS
        for element in FLOAT_ELEMENTS
        for vector_bit_width in AVX2_FLOAT_VECTOR_BIT_WIDTHS
        if vector_bit_width >= element.bit_width
    )
    return (
        *fast_scalar_rules,
        *exact_scalar_rules,
        *fast_vector_rules,
        *exact_vector_rules,
    )


def avx512_float_extrema_rules(
    descriptor_lookup: _DescriptorLookup,
) -> tuple[DescriptorRule, ...]:
    """Generates packed f32/f64 ZMM IEEE extrema families."""
    fast_rules = (
        _fast_float_extrema_rule(
            Vector(element.name, lanes=element.lane_count(512)),
            descriptor_lookup(
                f"x86.avx512.{FLOAT_EXTREMA_MNEMONICS[operation][element.name]}.zmm"
            ),
            _VECTOR_SOURCE_OPS[operation],
        )
        for operation in FLOAT_EXTREMA_OPERATIONS
        for element in FLOAT_ELEMENTS
    )
    exact_rules = (
        _float_extrema_rule(
            operation,
            Vector(element.name, lanes=element.lane_count(512)),
            _VECTOR_SOURCE_OPS[operation],
            _avx512_float_extrema_descriptors(operation, element, descriptor_lookup),
        )
        for operation in FLOAT_EXTREMA_OPERATIONS
        for element in FLOAT_ELEMENTS
    )
    return (*fast_rules, *exact_rules)


def avx512_fp16_float_extrema_rules(
    descriptor_lookup: _DescriptorLookup,
) -> tuple[DescriptorRule, ...]:
    """Generates scalar and packed AVX512-FP16 extrema families."""

    fast_scalar_rules = (
        _fast_float_extrema_rule(
            Scalar(FP16_ELEMENT.name),
            descriptor_lookup(
                "x86.avx512_fp16."
                f"{AVX512_FP16_SCALAR_FLOAT_EXTREMA_MNEMONICS[operation]}.xmm"
            ),
            _SCALAR_SOURCE_OPS[operation],
            priority=2,
        )
        for operation in FLOAT_EXTREMA_OPERATIONS
    )
    exact_scalar_rules = (
        _float_extrema_rule(
            operation,
            Scalar(FP16_ELEMENT.name),
            _SCALAR_SOURCE_OPS[operation],
            _avx512_fp16_float_extrema_descriptors(
                operation, 128, descriptor_lookup, scalar=True
            ),
            priority=1,
        )
        for operation in FLOAT_EXTREMA_OPERATIONS
    )
    fast_vector_rules = (
        _fast_float_extrema_rule(
            Vector(
                FP16_ELEMENT.name,
                lanes=FP16_ELEMENT.lane_count(vector_bit_width),
            ),
            descriptor_lookup(
                "x86.avx512_fp16."
                f"{AVX512_FP16_FLOAT_EXTREMA_MNEMONICS[operation]}."
                f"{_REGISTER_SUFFIXES[vector_bit_width]}"
            ),
            _VECTOR_SOURCE_OPS[operation],
            priority=2,
        )
        for operation in FLOAT_EXTREMA_OPERATIONS
        for vector_bit_width in AVX512_FP16_VECTOR_BIT_WIDTHS
    )
    exact_vector_rules = (
        _float_extrema_rule(
            operation,
            Vector(
                FP16_ELEMENT.name,
                lanes=FP16_ELEMENT.lane_count(vector_bit_width),
            ),
            _VECTOR_SOURCE_OPS[operation],
            _avx512_fp16_float_extrema_descriptors(
                operation, vector_bit_width, descriptor_lookup
            ),
            priority=1,
        )
        for operation in FLOAT_EXTREMA_OPERATIONS
        for vector_bit_width in AVX512_FP16_VECTOR_BIT_WIDTHS
    )
    return (
        *fast_scalar_rules,
        *exact_scalar_rules,
        *fast_vector_rules,
        *exact_vector_rules,
    )
