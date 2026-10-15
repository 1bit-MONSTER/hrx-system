# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""x86 scalar and packed floating-point root contract rules."""

from __future__ import annotations

from collections.abc import Sequence

from loom.dialect.scalar import math as scalar_math
from loom.dialect.vector import defs as vector
from loom.target.arch.x86.contracts.rule_builders import (
    DescriptorLookup as _DescriptorLookup,
)
from loom.target.arch.x86.contracts.rule_builders import (
    emit_descriptor_op as _op_emit,
)
from loom.target.arch.x86.contracts.rule_builders import (
    full_vector_type as _full_vector_type,
)
from loom.target.arch.x86.contracts.rule_builders import (
    value_type_guards as _typed_guards,
)
from loom.target.arch.x86.vector_families import FloatRootFamily
from loom.target.contracts import DescriptorRule, Guard, Scalar, ValueRef

_REGISTER_SUFFIXES = {
    32: "xmm",
    64: "xmm",
    128: "xmm",
    256: "ymm",
    512: "zmm",
}
_SCALAR_SOURCE_OPS = {
    "sqrtf": scalar_math.scalar_sqrtf,
    "rsqrtf": scalar_math.scalar_rsqrtf,
}
_VECTOR_SOURCE_OPS = {
    "sqrtf": vector.vector_sqrtf,
    "rsqrtf": vector.vector_rsqrtf,
}


def _fastmath_guards(family: FloatRootFamily) -> tuple[Guard, ...]:
    return (
        (Guard.instance_flags_has_all("fastmath", "afn"),) if family.approximate else ()
    )


def scalar_float_root_rules(
    descriptor_lookup: _DescriptorLookup,
    *,
    descriptor_key_prefix: str,
    families: Sequence[FloatRootFamily],
    priority: int = 0,
) -> tuple[DescriptorRule, ...]:
    """Builds scalar root rules with explicit upper-lane passthrough."""

    rules = []
    for family in families:
        descriptor = descriptor_lookup(f"{descriptor_key_prefix}.{family.mnemonic}.xmm")
        value_type = Scalar(family.element.name)
        rules.append(
            DescriptorRule(
                source_op=_SCALAR_SOURCE_OPS[family.source_operation],
                descriptor=descriptor,
                guards=(
                    *_typed_guards(("input", "result"), value_type),
                    *_fastmath_guards(family),
                ),
                emit=(
                    _op_emit(
                        descriptor=descriptor,
                        operands={
                            "passthrough": ValueRef.operand("input"),
                            "input": ValueRef.operand("input"),
                        },
                        results={"dst": ValueRef.result("result")},
                    ),
                ),
                priority=priority,
            )
        )
    return tuple(rules)


def vector_float_root_rules(
    descriptor_lookup: _DescriptorLookup,
    *,
    descriptor_key_prefix: str,
    families: Sequence[FloatRootFamily],
    vector_bit_widths: Sequence[int],
    priority: int = 0,
) -> tuple[DescriptorRule, ...]:
    """Builds packed root rules for each logical source packet width."""

    rules = []
    for family in families:
        for vector_bit_width in vector_bit_widths:
            if vector_bit_width < family.element.bit_width:
                continue
            descriptor = descriptor_lookup(
                f"{descriptor_key_prefix}.{family.mnemonic}."
                f"{_REGISTER_SUFFIXES[vector_bit_width]}"
            )
            value_type = _full_vector_type(family.element, vector_bit_width)
            rules.append(
                DescriptorRule(
                    source_op=_VECTOR_SOURCE_OPS[family.source_operation],
                    descriptor=descriptor,
                    guards=(
                        *_typed_guards(("input", "result"), value_type),
                        *_fastmath_guards(family),
                    ),
                    emit=(
                        _op_emit(
                            descriptor=descriptor,
                            operands={"input": ValueRef.operand("input")},
                            results={"dst": ValueRef.result("result")},
                        ),
                    ),
                    priority=priority,
                )
            )
    return tuple(rules)
