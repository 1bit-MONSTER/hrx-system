# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""x86 packed-integer vector conversion contract rules."""

from __future__ import annotations

from loom.dialect.vector import defs as vector
from loom.target.arch.x86.contracts.rule_builders import (
    DescriptorLookup as _DescriptorLookup,
)
from loom.target.arch.x86.contracts.rule_builders import emit_descriptor_op as _op_emit
from loom.target.arch.x86.vector_families import (
    X86_VECTOR_INTEGER_WIDENING_FAMILIES,
)
from loom.target.contracts import DescriptorRule, Guard, ValueRef, Vector

_REGISTER_SUFFIXES = {128: "xmm", 256: "ymm", 512: "zmm"}


def _physical_register_suffix(vector_bit_width: int) -> str:
    return _REGISTER_SUFFIXES[max(128, vector_bit_width)]


def vector_integer_widening_rules(
    descriptor_lookup: _DescriptorLookup,
    *,
    descriptor_key_prefix: str,
    source_vector_bit_widths: tuple[int, ...],
    result_vector_bit_widths: tuple[int, ...],
) -> tuple[DescriptorRule, ...]:
    """Returns native rules between supported one-register vector carriers."""

    rules: list[DescriptorRule] = []
    for family in X86_VECTOR_INTEGER_WIDENING_FAMILIES:
        for source_op, mnemonic in (
            (vector.vector_extsi, family.signed_mnemonic),
            (vector.vector_extui, family.unsigned_mnemonic),
        ):
            for result_vector_bit_width in result_vector_bit_widths:
                if result_vector_bit_width < family.result.bit_width:
                    continue
                lane_count = result_vector_bit_width // family.result.bit_width
                source_vector_bit_width = lane_count * family.source.bit_width
                if source_vector_bit_width not in source_vector_bit_widths:
                    continue
                source_type = Vector(family.source.name, lanes=lane_count)
                result_type = Vector(family.result.name, lanes=lane_count)
                source_suffix = _physical_register_suffix(source_vector_bit_width)
                descriptor = descriptor_lookup(
                    f"{descriptor_key_prefix}.{mnemonic}."
                    f"{_physical_register_suffix(result_vector_bit_width)}."
                    f"{source_suffix}"
                )
                rules.append(
                    DescriptorRule(
                        source_op=source_op,
                        descriptor=descriptor,
                        guards=(
                            Guard.value_type("input", source_type),
                            Guard.value_type("result", result_type),
                            Guard.value_static_element_count_eq("input", "result"),
                        ),
                        emit=(
                            _op_emit(
                                descriptor=descriptor,
                                operands={"source": ValueRef.operand("input")},
                                results={"dst": ValueRef.result("result")},
                            ),
                        ),
                        report_key="x86.vector_integer_widen.native",
                    )
                )
    return tuple(rules)
