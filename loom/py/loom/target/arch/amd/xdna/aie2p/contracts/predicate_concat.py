# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""AIE2P packed-predicate concat contracts."""

from dataclasses import dataclass

from loom.dialect.vector import defs as vector
from loom.target.arch.amd.xdna.aie2p.contracts.predicate_words import (
    _carrier_emits,
    _constant_emit,
    _descriptor,
    _maximum_lane_count,
    _slice_carriers,
    _source_word,
    _word_count,
    _WordContribution,
)
from loom.target.contracts import (
    ContractEmit,
    DescriptorEmitForm,
    DescriptorResultType,
    DescriptorRule,
    EmitDescriptorOp,
    EmitRegisterConcat,
    Guard,
    TypePattern,
    ValueRef,
    ValueTypeProject,
    Vector,
)

_NARROW_PREDICATE = Vector("i1", minimum_lanes=1, maximum_lanes=64)
_WIDE_PREDICATE = Vector("i1", minimum_lanes=65, maximum_lanes=128)


@dataclass(frozen=True, slots=True)
class _PredicateConcatSpec:
    """One invariant partition of the predicate concat family."""

    left_type: TypePattern
    right_type: TypePattern
    result_type: TypePattern
    boundary_word: int
    exact_boundary: bool = False


def _predicate_concat_guards(
    left_type: TypePattern,
    right_type: TypePattern,
    result_type: TypePattern,
) -> tuple[Guard, ...]:
    return (
        Guard.i64_range("axis", 0, 0),
        Guard.operand_segment_count("inputs", 2),
        Guard.value_type("inputs", left_type, element=0),
        Guard.value_type("inputs", right_type, element=1),
        Guard.value_type("result", result_type),
    )


def _boundary_emits(
    left_word: _WordContribution,
    left: ValueRef,
    *,
    boundary_word: int,
    needs_carry_shift: bool,
) -> tuple[tuple[EmitDescriptorOp, ...], _WordContribution, ValueRef, ValueRef | None]:
    """Masks one partial left word and materializes its merge shifts."""

    lane_base = boundary_word * 32
    shift = ValueRef.temporary("concat_shift")
    carry_shift = (
        ValueRef.temporary("concat_carry_shift") if needs_carry_shift else None
    )
    mask = ValueRef.temporary("concat_mask")
    masked_left = ValueRef.temporary("concat_masked_left")
    emits = [
        _constant_emit(
            shift,
            ValueTypeProject.static_dim_scaled(left, scale=1, addend=-lane_base),
        )
    ]
    if carry_shift is not None:
        emits.append(
            _constant_emit(
                carry_shift,
                ValueTypeProject.static_dim_scaled(
                    left,
                    scale=1,
                    addend=-(lane_base + 32),
                ),
            )
        )
    emits.append(
        _constant_emit(
            mask,
            ValueTypeProject.static_dim_low_bits_mask(left, addend=-lane_base),
            full_width=True,
        )
    )
    emits.append(
        EmitDescriptorOp(
            descriptor=_descriptor(
                f"amd.xdna.aie2p.predicate.mask.{left_word.part}.to.low32"
            ),
            operands={"s0": left_word.source, "s1": mask},
            results={"d0": masked_left},
            result_types={"d0": DescriptorResultType()},
            form=DescriptorEmitForm.OP,
        )
    )
    return (
        tuple(emits),
        _WordContribution(masked_left, "low32"),
        shift,
        carry_shift,
    )


def _positioned_words(
    spec: _PredicateConcatSpec,
    left: ValueRef,
    left_carriers: tuple[ValueRef, ...],
    right_carriers: tuple[ValueRef, ...],
) -> tuple[tuple[EmitDescriptorOp, ...], tuple[tuple[_WordContribution, ...], ...]]:
    """Positions source words in the physical words of the result."""

    result_word_count = _word_count(spec.result_type)
    right_word_count = _word_count(spec.right_type)
    words: list[tuple[_WordContribution, ...]] = [
        (_source_word(left_carriers, index),) for index in range(spec.boundary_word)
    ]
    words.extend(() for _ in range(result_word_count - len(words)))

    if spec.exact_boundary:
        zero = ValueRef.temporary("concat_shift")
        for result_word in range(spec.boundary_word, result_word_count):
            right_word = result_word - spec.boundary_word
            if right_word >= right_word_count:
                break
            source = _source_word(right_carriers, right_word)
            words[result_word] = (_WordContribution(source.source, source.part, zero),)
        return (_constant_emit(zero, 0),), tuple(words)

    boundary_emits, masked_left, shift, carry_shift = _boundary_emits(
        _source_word(left_carriers, spec.boundary_word),
        left,
        boundary_word=spec.boundary_word,
        needs_carry_shift=result_word_count > spec.boundary_word + 1,
    )
    words[spec.boundary_word] = (
        masked_left,
        _WordContribution(right_carriers[0], "low32", shift),
    )
    for result_word in range(spec.boundary_word + 1, result_word_count):
        assert carry_shift is not None
        right_word = result_word - spec.boundary_word
        previous = _source_word(right_carriers, right_word - 1)
        contributions = [_WordContribution(previous.source, previous.part, carry_shift)]
        if right_word < right_word_count:
            current = _source_word(right_carriers, right_word)
            contributions.append(_WordContribution(current.source, current.part, shift))
        words[result_word] = tuple(contributions)
    return boundary_emits, tuple(words)


def _predicate_concat_rule(spec: _PredicateConcatSpec) -> DescriptorRule:
    left = ValueRef.operand("inputs", element=0)
    right = ValueRef.operand("inputs", element=1)
    guards = _predicate_concat_guards(
        spec.left_type,
        spec.right_type,
        spec.result_type,
    )

    # A whole predicate carrier needs no scalar composition.
    if spec.exact_boundary and spec.boundary_word == 2:
        return DescriptorRule(
            source_op=vector.vector_concat,
            guards=guards,
            emit=(
                EmitRegisterConcat(
                    sources=(left, right),
                    result=ValueRef.result("result"),
                ),
            ),
        )

    left_slices, left_carriers = _slice_carriers(
        left,
        spec.left_type,
        temporary_prefix="left_",
    )
    right_slices, right_carriers = _slice_carriers(
        right,
        spec.right_type,
        temporary_prefix="right_",
    )
    boundary_emits, words = _positioned_words(
        spec,
        left,
        left_carriers,
        right_carriers,
    )
    result_carrier_count = (_maximum_lane_count(spec.result_type) + 63) // 64
    boundary_carrier = spec.boundary_word // 2
    result_carriers = list(left_carriers[:boundary_carrier])
    emits: list[ContractEmit] = [
        *left_slices,
        *right_slices,
        *boundary_emits,
    ]
    for carrier_index in range(boundary_carrier, result_carrier_count):
        result_is_temporary = result_carrier_count > 1
        result = (
            ValueRef.temporary(f"result_carrier_{carrier_index}")
            if result_is_temporary
            else ValueRef.result("result")
        )
        low_word = carrier_index * 2
        high_word = low_word + 1
        emits.extend(
            _carrier_emits(
                words[low_word],
                words[high_word] if high_word < len(words) else (),
                result,
                temporary_prefix=f"result_{carrier_index}_",
                result_is_temporary=result_is_temporary,
            )
        )
        result_carriers.append(result)

    if result_carrier_count > 1:
        emits.append(
            EmitRegisterConcat(
                sources=tuple(result_carriers),
                result=ValueRef.result("result"),
            )
        )
    primary_descriptor = next(
        emit.descriptor
        for emit in reversed(emits)
        if isinstance(emit, EmitDescriptorOp)
    )
    return DescriptorRule(
        source_op=vector.vector_concat,
        descriptor=primary_descriptor,
        guards=guards,
        emit=tuple(emits),
    )


_PREDICATE_CONCAT_SPECS = (
    # The left predicate ends in output word 0.
    _PredicateConcatSpec(
        Vector("i1", minimum_lanes=1, maximum_lanes=31),
        _NARROW_PREDICATE,
        Vector("i1", minimum_lanes=2, maximum_lanes=32),
        boundary_word=0,
    ),
    _PredicateConcatSpec(
        Vector("i1", minimum_lanes=1, maximum_lanes=31),
        _NARROW_PREDICATE,
        Vector("i1", minimum_lanes=33, maximum_lanes=64),
        boundary_word=0,
    ),
    _PredicateConcatSpec(
        Vector("i1", minimum_lanes=1, maximum_lanes=31),
        _NARROW_PREDICATE,
        Vector("i1", minimum_lanes=65, maximum_lanes=95),
        boundary_word=0,
    ),
    _PredicateConcatSpec(
        Vector("i1", minimum_lanes=1, maximum_lanes=31),
        _WIDE_PREDICATE,
        Vector("i1", minimum_lanes=66, maximum_lanes=128),
        boundary_word=0,
    ),
    # The left predicate ends exactly at output word 1.
    _PredicateConcatSpec(
        Vector("i1", lanes=32),
        _NARROW_PREDICATE,
        Vector("i1", minimum_lanes=33, maximum_lanes=64),
        boundary_word=1,
        exact_boundary=True,
    ),
    _PredicateConcatSpec(
        Vector("i1", lanes=32),
        _NARROW_PREDICATE,
        Vector("i1", minimum_lanes=65, maximum_lanes=96),
        boundary_word=1,
        exact_boundary=True,
    ),
    _PredicateConcatSpec(
        Vector("i1", lanes=32),
        _WIDE_PREDICATE,
        Vector("i1", minimum_lanes=97, maximum_lanes=128),
        boundary_word=1,
        exact_boundary=True,
    ),
    # The left predicate ends in output word 1.
    _PredicateConcatSpec(
        Vector("i1", minimum_lanes=33, maximum_lanes=63),
        _NARROW_PREDICATE,
        Vector("i1", minimum_lanes=34, maximum_lanes=64),
        boundary_word=1,
    ),
    _PredicateConcatSpec(
        Vector("i1", minimum_lanes=33, maximum_lanes=63),
        _NARROW_PREDICATE,
        Vector("i1", minimum_lanes=65, maximum_lanes=127),
        boundary_word=1,
    ),
    _PredicateConcatSpec(
        Vector("i1", minimum_lanes=33, maximum_lanes=63),
        _WIDE_PREDICATE,
        Vector("i1", minimum_lanes=98, maximum_lanes=128),
        boundary_word=1,
    ),
    # Sixty-four left lanes already occupy one complete carrier.
    _PredicateConcatSpec(
        Vector("i1", lanes=64),
        _NARROW_PREDICATE,
        _WIDE_PREDICATE,
        boundary_word=2,
        exact_boundary=True,
    ),
    # The left predicate ends in output word 2.
    _PredicateConcatSpec(
        Vector("i1", minimum_lanes=65, maximum_lanes=95),
        _NARROW_PREDICATE,
        Vector("i1", minimum_lanes=66, maximum_lanes=96),
        boundary_word=2,
    ),
    _PredicateConcatSpec(
        Vector("i1", minimum_lanes=65, maximum_lanes=95),
        _NARROW_PREDICATE,
        Vector("i1", minimum_lanes=97, maximum_lanes=128),
        boundary_word=2,
    ),
    # The left predicate ends exactly at output word 3.
    _PredicateConcatSpec(
        Vector("i1", lanes=96),
        _NARROW_PREDICATE,
        Vector("i1", minimum_lanes=97, maximum_lanes=128),
        boundary_word=3,
        exact_boundary=True,
    ),
    # The left predicate ends in output word 3.
    _PredicateConcatSpec(
        Vector("i1", minimum_lanes=97, maximum_lanes=127),
        _NARROW_PREDICATE,
        Vector("i1", minimum_lanes=98, maximum_lanes=128),
        boundary_word=3,
    ),
)


AIE2P_PREDICATE_CONCAT_RULES = tuple(
    _predicate_concat_rule(spec) for spec in _PREDICATE_CONCAT_SPECS
)
