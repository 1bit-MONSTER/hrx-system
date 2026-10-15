# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Shared builders for x86 source-to-low contract rules."""

from __future__ import annotations

from collections.abc import Callable, Iterable, Mapping

from loom.target.arch.x86.vector_families import VectorElement
from loom.target.contracts import (
    AttrProject,
    DescriptorEmitForm,
    DescriptorResultType,
    EmitDescriptorOp,
    Guard,
    ReadOnlyDataLiteral,
    TypePattern,
    ValueRef,
    Vector,
)
from loom.target.low_descriptors import Descriptor

DescriptorLookup = Callable[[str], Descriptor]

_ZMM_SWAP_256_BIT_HALVES_CONTROL = 0x4E
_ZMM_SWAP_128_BIT_QUARTERS_CONTROL = 0xB1


def emit_descriptor_op(
    *,
    descriptor: Descriptor,
    operands: Mapping[str, ValueRef] | None = None,
    results: Mapping[str, ValueRef] | None = None,
    result_types: Mapping[str, TypePattern | DescriptorResultType] | None = None,
    immediates: Mapping[str, AttrProject | ReadOnlyDataLiteral | int] | None = None,
) -> EmitDescriptorOp:
    return EmitDescriptorOp(
        descriptor=descriptor,
        operands={} if operands is None else dict(operands),
        results={} if results is None else dict(results),
        result_types=None if result_types is None else dict(result_types),
        immediates={} if immediates is None else dict(immediates),
        form=DescriptorEmitForm.OP,
    )


def value_type_guards(
    fields: Iterable[str],
    type_pattern: TypePattern,
) -> tuple[Guard, ...]:
    return tuple(Guard.value_type(field, type_pattern) for field in fields)


def full_vector_type(element: VectorElement, vector_bit_width: int) -> TypePattern:
    return Vector(element.name, lanes=element.lane_count(vector_bit_width))


def zmm_reduction_emit_chain(
    input_value: ValueRef,
    combine: Descriptor,
    element_bit_width: int,
    descriptor_lookup: DescriptorLookup,
    *,
    temporary_prefix: str = "",
) -> tuple[tuple[EmitDescriptorOp, ...], ValueRef, tuple[Descriptor, ...]]:
    """Reduces an associative, commutative ZMM value.

    The scalar result is replicated in lane zero of each XMM quarter.
    """
    shuffle = descriptor_lookup("x86.avx512.vshufi64x2.zmm")
    shift = descriptor_lookup("x86.avx512.vpsrldq.zmm")
    emits: list[EmitDescriptorOp] = []
    reduced = input_value
    for control, stage_name in (
        (_ZMM_SWAP_256_BIT_HALVES_CONTROL, "half"),
        (_ZMM_SWAP_128_BIT_QUARTERS_CONTROL, "quarter"),
    ):
        shuffled = ValueRef.temporary(f"{temporary_prefix}{stage_name}_shuffled")
        next_reduced = ValueRef.temporary(f"{temporary_prefix}{stage_name}_combined")
        emits.extend(
            (
                emit_descriptor_op(
                    descriptor=shuffle,
                    operands={"lhs": reduced, "rhs": reduced},
                    results={"dst": shuffled},
                    result_types={"dst": DescriptorResultType()},
                    immediates={"control": control},
                ),
                emit_descriptor_op(
                    descriptor=combine,
                    operands={"lhs": reduced, "rhs": shuffled},
                    results={"dst": next_reduced},
                    result_types={"dst": DescriptorResultType()},
                ),
            )
        )
        reduced = next_reduced

    shift_bytes = 8
    ordinal = 0
    while shift_bytes >= element_bit_width // 8:
        shifted = ValueRef.temporary(f"{temporary_prefix}shifted{ordinal}")
        next_reduced = ValueRef.temporary(f"{temporary_prefix}reduced{ordinal}")
        emits.extend(
            (
                emit_descriptor_op(
                    descriptor=shift,
                    operands={"source": reduced},
                    results={"dst": shifted},
                    result_types={"dst": DescriptorResultType()},
                    immediates={"bytes": shift_bytes},
                ),
                emit_descriptor_op(
                    descriptor=combine,
                    operands={"lhs": reduced, "rhs": shifted},
                    results={"dst": next_reduced},
                    result_types={"dst": DescriptorResultType()},
                ),
            )
        )
        reduced = next_reduced
        ordinal += 1
        shift_bytes //= 2

    return tuple(emits), reduced, (shuffle, shift)
