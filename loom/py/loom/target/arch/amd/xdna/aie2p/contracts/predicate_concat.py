# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""AIE2P packed-predicate concat contracts."""

from dataclasses import dataclass
from typing import Literal

from loom.dialect.vector import defs as vector
from loom.target.arch.amd.xdna.aie2p.core_descriptors import (
    AIE2P_CORE_DESCRIPTOR_SET,
)
from loom.target.contracts import (
    ContractEmit,
    DescriptorEmitForm,
    DescriptorResultType,
    DescriptorRule,
    EmitDescriptorOp,
    EmitRegisterConcat,
    EmitRegisterSlice,
    Guard,
    TypePattern,
    ValueRef,
    ValueTypeProject,
    Vector,
    descriptor_by_key,
)
from loom.target.low_descriptors import Descriptor

_NARROW_PREDICATE = Vector("i1", minimum_lanes=1, maximum_lanes=64)
_WIDE_PREDICATE = Vector("i1", minimum_lanes=65, maximum_lanes=128)

_PredicateWordPart = Literal["low32", "high32"]


@dataclass(frozen=True, slots=True)
class _WordContribution:
    """One source predicate word positioned in an output word."""

    source: ValueRef
    part: _PredicateWordPart
    shift: ValueRef | None = None


@dataclass(frozen=True, slots=True)
class _PredicateConcatSpec:
    """One invariant partition of the predicate concat family."""

    left_type: TypePattern
    right_type: TypePattern
    result_type: TypePattern
    boundary_word: int
    exact_boundary: bool = False


def _descriptor(key: str) -> Descriptor:
    return descriptor_by_key(AIE2P_CORE_DESCRIPTOR_SET, key)


def _maximum_lane_count(type_pattern: TypePattern) -> int:
    maximum = (
        type_pattern.lanes
        if type_pattern.lanes is not None
        else type_pattern.maximum_lanes
    )
    assert isinstance(maximum, int)
    return maximum


def _word_count(type_pattern: TypePattern) -> int:
    return (_maximum_lane_count(type_pattern) + 31) // 32


def _word_part(word_index: int) -> _PredicateWordPart:
    return "low32" if word_index % 2 == 0 else "high32"


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


def _result_types(
    field: str, *, result_is_temporary: bool
) -> dict[str, DescriptorResultType] | None:
    return {field: DescriptorResultType()} if result_is_temporary else None


def _constant_emit(
    result: ValueRef,
    value: int | ValueTypeProject,
    *,
    wide: bool = False,
) -> EmitDescriptorOp:
    return EmitDescriptorOp(
        descriptor=_descriptor(
            "amd.xdna.aie2p.constant.i32.mova"
            if wide
            else "amd.xdna.aie2p.constant.i32.short"
        ),
        results={"dst": result},
        result_types={"dst": DescriptorResultType()},
        immediates={"i": value},
        form=DescriptorEmitForm.CONST,
    )


def _slice_carriers(
    source: ValueRef,
    source_type: TypePattern,
    *,
    temporary_prefix: str,
) -> tuple[tuple[EmitRegisterSlice, ...], tuple[ValueRef, ...]]:
    if _maximum_lane_count(source_type) <= 64:
        return (), (source,)
    low = ValueRef.temporary(f"{temporary_prefix}carrier_0")
    high = ValueRef.temporary(f"{temporary_prefix}carrier_1")
    return (
        (
            EmitRegisterSlice(source=source, result=low, unit_count=1),
            EmitRegisterSlice(
                source=source,
                result=high,
                unit_offset=1,
                unit_count=1,
            ),
        ),
        (low, high),
    )


def _source_word(carriers: tuple[ValueRef, ...], word_index: int) -> _WordContribution:
    return _WordContribution(carriers[word_index // 2], _word_part(word_index))


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
            wide=True,
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


def _shift_descriptor_key(
    source_part: _PredicateWordPart,
    destination_part: _PredicateWordPart,
) -> str:
    key = f"amd.xdna.aie2p.predicate.shift.{source_part}"
    return key if destination_part == "low32" else f"{key}.to.high32"


def _shift_word_emit(
    contribution: _WordContribution,
    *,
    destination_part: _PredicateWordPart,
    result: ValueRef,
    storage: ValueRef | None = None,
    result_is_temporary: bool,
) -> EmitDescriptorOp:
    assert contribution.shift is not None
    operands = {"s0": contribution.source, "s1": contribution.shift}
    if destination_part == "high32":
        assert storage is not None
        operands["storage"] = storage
    return EmitDescriptorOp(
        descriptor=_descriptor(
            _shift_descriptor_key(contribution.part, destination_part)
        ),
        operands=operands,
        results={"d0": result},
        result_types=_result_types("d0", result_is_temporary=result_is_temporary),
        form=DescriptorEmitForm.OP,
    )


def _low_word_values(
    contributions: tuple[_WordContribution, ...],
    *,
    temporary_prefix: str,
) -> tuple[tuple[EmitDescriptorOp, ...], tuple[ValueRef, ...]]:
    """Positions contributions in independent low-word temporaries."""

    assert 1 <= len(contributions) <= 2
    emits: list[EmitDescriptorOp] = []
    values: list[ValueRef] = []
    for index, contribution in enumerate(contributions):
        if contribution.part == "low32" and contribution.shift is None:
            values.append(contribution.source)
            continue
        shifted = ValueRef.temporary(f"{temporary_prefix}shifted_{index}")
        emits.append(
            _shift_word_emit(
                contribution,
                destination_part="low32",
                result=shifted,
                result_is_temporary=True,
            )
        )
        values.append(shifted)
    return tuple(emits), tuple(values)


def _low_word_emits(
    contributions: tuple[_WordContribution, ...],
    *,
    temporary_prefix: str,
) -> tuple[tuple[EmitDescriptorOp, ...], ValueRef]:
    """Builds one low predicate word, retaining partial eL storage."""

    value_emits, values = _low_word_values(
        contributions,
        temporary_prefix=temporary_prefix,
    )
    if len(values) == 1:
        return value_emits, values[0]

    combined = ValueRef.temporary(f"{temporary_prefix}combined")
    emits = [
        *value_emits,
        EmitDescriptorOp(
            descriptor=_descriptor("amd.xdna.aie2p.predicate.or.low32.rhs_tied"),
            operands={"s0": values[0], "s1": values[1]},
            results={"d0": combined},
            result_types={"d0": DescriptorResultType()},
            form=DescriptorEmitForm.OP,
        ),
    ]
    return tuple(emits), combined


def _carrier_emits(
    low_contributions: tuple[_WordContribution, ...],
    high_contributions: tuple[_WordContribution, ...],
    result: ValueRef,
    *,
    temporary_prefix: str,
    result_is_temporary: bool,
) -> tuple[EmitDescriptorOp, ...]:
    """Builds one complete eL carrier from at most two inputs per word."""

    low_emits, low = _low_word_emits(
        low_contributions,
        temporary_prefix=f"{temporary_prefix}low_",
    )
    emits = list(low_emits)
    if not high_contributions:
        emits.append(
            EmitDescriptorOp(
                descriptor=_descriptor("amd.xdna.aie2p.predicate.complete.zero.high32"),
                operands={"storage": low},
                results={"dst": result},
                result_types=_result_types(
                    "dst", result_is_temporary=result_is_temporary
                ),
                immediates={"i": 0},
                form=DescriptorEmitForm.OP,
            )
        )
        return tuple(emits)

    assert len(high_contributions) <= 2
    if len(high_contributions) == 2:
        high_emits, high_values = _low_word_values(
            high_contributions,
            temporary_prefix=f"{temporary_prefix}high_",
        )
        emits.extend(high_emits)
        emits.append(
            EmitDescriptorOp(
                descriptor=_descriptor("amd.xdna.aie2p.predicate.or.low32.to.high32"),
                operands={
                    "s0": high_values[0],
                    "s1": high_values[1],
                    "storage": low,
                },
                results={"d0": result},
                result_types=_result_types(
                    "d0", result_is_temporary=result_is_temporary
                ),
                form=DescriptorEmitForm.OP,
            )
        )
        return tuple(emits)

    contribution = high_contributions[0]
    if contribution.shift is not None:
        emits.append(
            _shift_word_emit(
                contribution,
                destination_part="high32",
                result=result,
                storage=low,
                result_is_temporary=result_is_temporary,
            )
        )
        return tuple(emits)

    source_part = contribution.part
    emits.append(
        EmitDescriptorOp(
            descriptor=_descriptor(
                "amd.xdna.aie2p.predicate.or.low32.to.high32"
                if source_part == "low32"
                else "amd.xdna.aie2p.predicate.or.high32"
            ),
            operands={
                "s0": contribution.source,
                "s1": contribution.source,
                "storage": low,
            },
            results={"d0": result},
            result_types=_result_types("d0", result_is_temporary=result_is_temporary),
            form=DescriptorEmitForm.OP,
        )
    )
    return tuple(emits)


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
