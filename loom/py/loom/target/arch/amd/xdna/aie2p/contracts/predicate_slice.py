# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""AIE2P packed-predicate slice contracts."""

from dataclasses import dataclass

from loom.dialect.vector import defs as vector
from loom.target.arch.amd.xdna.aie2p.contracts.predicate_words import (
    _carrier_emits,
    _constant_emit,
    _slice_carriers,
    _source_word,
    _word_count,
    _word_part,
    _WordContribution,
)
from loom.target.contracts import (
    AttrProject,
    ContractEmit,
    DescriptorRule,
    EmitDescriptorOp,
    EmitRegisterConcat,
    EmitRegisterSlice,
    Guard,
    TypePattern,
    ValueRef,
    Vector,
)

_NARROW_PREDICATE = Vector("i1", minimum_lanes=1, maximum_lanes=64)
_WIDE_PREDICATE = Vector("i1", minimum_lanes=65, maximum_lanes=128)
_RESULT_WORD_TYPES = tuple(
    Vector(
        "i1",
        minimum_lanes=word_index * 32 + 1,
        maximum_lanes=(word_index + 1) * 32,
    )
    for word_index in range(4)
)


@dataclass(frozen=True, slots=True)
class _PredicateSliceSpec:
    """One source-word and result-width partition of predicate slicing."""

    source_type: TypePattern
    result_type: TypePattern
    source_word: int
    offset_minimum: int
    offset_maximum: int
    aligned: bool = False


def _predicate_slice_guards(spec: _PredicateSliceSpec) -> tuple[Guard, ...]:
    return (
        Guard.value_type("source", spec.source_type),
        Guard.value_type("result", spec.result_type),
        Guard.operand_segment_count("offsets", 0),
        Guard.i64_array_count("static_offsets", 1),
        Guard.i64_array_element_range(
            "static_offsets",
            element=0,
            minimum=spec.offset_minimum,
            maximum=spec.offset_maximum,
        ),
    )


def _aligned_words(
    spec: _PredicateSliceSpec,
    source_carriers: tuple[ValueRef, ...],
) -> tuple[tuple[EmitDescriptorOp, ...], tuple[tuple[_WordContribution, ...], ...]]:
    zero = ValueRef.temporary("slice_shift")
    words = []
    for result_word in range(_word_count(spec.result_type)):
        source_word_index = spec.source_word + result_word
        source = _source_word(source_carriers, source_word_index)
        shift = zero if source.part != _word_part(result_word) else None
        words.append((_WordContribution(source.source, source.part, shift),))
    return (_constant_emit(zero, 0),), tuple(words)


def _unaligned_words(
    spec: _PredicateSliceSpec,
    source_carriers: tuple[ValueRef, ...],
) -> tuple[tuple[EmitDescriptorOp, ...], tuple[tuple[_WordContribution, ...], ...]]:
    source_word_count = _word_count(spec.source_type)
    result_word_count = _word_count(spec.result_type)
    word_base = spec.source_word * 32
    right_shift = ValueRef.temporary("slice_right_shift")
    left_shift = ValueRef.temporary("slice_left_shift")
    emits = [
        _constant_emit(
            right_shift,
            AttrProject.i64_array_literal_minus_element(
                "static_offsets",
                element=0,
                literal=word_base,
            ),
        ),
    ]
    if spec.source_word + 1 < source_word_count:
        emits.append(
            _constant_emit(
                left_shift,
                AttrProject.i64_array_literal_minus_element(
                    "static_offsets",
                    element=0,
                    literal=word_base + 32,
                ),
            )
        )
    words: list[tuple[_WordContribution, ...]] = []
    for result_word in range(result_word_count):
        source_word_index = spec.source_word + result_word
        source = _source_word(source_carriers, source_word_index)
        contributions = [
            _WordContribution(source.source, source.part, right_shift),
        ]
        if source_word_index + 1 < source_word_count:
            carry = _source_word(source_carriers, source_word_index + 1)
            contributions.append(
                _WordContribution(carry.source, carry.part, left_shift)
            )
        words.append(tuple(contributions))
    return tuple(emits), tuple(words)


def _noncrossing_slice_rule(spec: _PredicateSliceSpec) -> DescriptorRule:
    """Keeps one-word packet intervals on their minimal path."""

    source = ValueRef.operand("source")
    source_slices: tuple[EmitRegisterSlice, ...] = ()
    source_carrier = source
    if spec.source_type == _WIDE_PREDICATE:
        source_carrier = ValueRef.temporary("source_carrier")
        source_slices = (
            EmitRegisterSlice(
                source=source,
                result=source_carrier,
                unit_offset=spec.source_word // 2,
                unit_count=1,
            ),
        )
    shift = ValueRef.temporary("slice_shift")
    packet = _WordContribution(source_carrier, _word_part(spec.source_word))
    emits = (
        *source_slices,
        _constant_emit(
            shift,
            AttrProject.i64_array_literal_minus_element(
                "static_offsets",
                element=0,
                literal=spec.source_word * 32,
            ),
        ),
        *_carrier_emits(
            (_WordContribution(packet.source, packet.part, shift),),
            (),
            ValueRef.result("result"),
            temporary_prefix="result_0_",
            result_is_temporary=False,
        ),
    )
    return DescriptorRule(
        source_op=vector.vector_slice,
        descriptor=emits[-1].descriptor,
        guards=_predicate_slice_guards(spec),
        emit=emits,
        priority=1,
    )


def _predicate_slice_rule(spec: _PredicateSliceSpec) -> DescriptorRule:
    source = ValueRef.operand("source")
    source_slices, source_carriers = _slice_carriers(
        source,
        spec.source_type,
        temporary_prefix="source_",
    )
    shift_emits, words = (
        _aligned_words(spec, source_carriers)
        if spec.aligned
        else _unaligned_words(spec, source_carriers)
    )
    result_carrier_count = (_word_count(spec.result_type) + 1) // 2
    result_carriers: list[ValueRef] = []
    emits: list[ContractEmit] = [*source_slices, *shift_emits]
    for carrier_index in range(result_carrier_count):
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
        source_op=vector.vector_slice,
        descriptor=primary_descriptor,
        guards=_predicate_slice_guards(spec),
        emit=tuple(emits),
    )


def _predicate_slice_specs() -> tuple[_PredicateSliceSpec, ...]:
    specs = []
    for source_type in (_NARROW_PREDICATE, _WIDE_PREDICATE):
        source_word_count = _word_count(source_type)
        for source_word in range(source_word_count):
            maximum_result_words = source_word_count - source_word
            word_base = source_word * 32
            for result_word_count in range(1, maximum_result_words + 1):
                result_type = _RESULT_WORD_TYPES[result_word_count - 1]
                if source_word not in (0, 2):
                    specs.append(
                        _PredicateSliceSpec(
                            source_type,
                            result_type,
                            source_word,
                            word_base,
                            word_base,
                            aligned=True,
                        )
                    )
                specs.append(
                    _PredicateSliceSpec(
                        source_type,
                        result_type,
                        source_word,
                        word_base + 1,
                        word_base + 31,
                    )
                )
    return tuple(specs)


def _noncrossing_slice_specs() -> tuple[_PredicateSliceSpec, ...]:
    specs = []
    # Eight predicate lanes are the smallest target packet. These rectangular
    # partitions retain a one-word path whenever that packet granularity proves
    # the result ends before the next 32-bit source word.
    result_partitions = (
        (1, 8, 24),
        (9, 16, 16),
        (17, 24, 8),
        (25, 31, 1),
    )
    for source_type in (_NARROW_PREDICATE, _WIDE_PREDICATE):
        source_word_count = _word_count(source_type)
        for source_word in range(source_word_count - 1):
            word_base = source_word * 32
            for (
                result_minimum,
                result_maximum,
                maximum_word_offset,
            ) in result_partitions:
                specs.append(
                    _PredicateSliceSpec(
                        source_type,
                        Vector(
                            "i1",
                            minimum_lanes=result_minimum,
                            maximum_lanes=result_maximum,
                        ),
                        source_word,
                        word_base + 1,
                        word_base + maximum_word_offset,
                    )
                )
    return tuple(specs)


AIE2P_PREDICATE_SLICE_RULES = (
    *(_noncrossing_slice_rule(spec) for spec in _noncrossing_slice_specs()),
    *(_predicate_slice_rule(spec) for spec in _predicate_slice_specs()),
)
