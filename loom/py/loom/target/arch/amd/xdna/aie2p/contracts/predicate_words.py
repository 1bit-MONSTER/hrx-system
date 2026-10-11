# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""AIE2P packed-predicate word composition."""

from dataclasses import dataclass
from typing import Literal

from loom.target.arch.amd.xdna.aie2p.core_descriptors import (
    AIE2P_CORE_DESCRIPTOR_SET,
)
from loom.target.contracts import (
    AttrProject,
    DescriptorEmitForm,
    DescriptorResultType,
    EmitDescriptorOp,
    EmitRegisterSlice,
    TypePattern,
    ValueRef,
    ValueTypeProject,
    descriptor_by_key,
)
from loom.target.low_descriptors import Descriptor

_PredicateWordPart = Literal["low32", "high32"]


@dataclass(frozen=True, slots=True)
class _WordContribution:
    """One source predicate word positioned in an output word."""

    source: ValueRef
    part: _PredicateWordPart
    shift: ValueRef | None = None


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


def _result_types(
    field: str, *, result_is_temporary: bool
) -> dict[str, DescriptorResultType] | None:
    return {field: DescriptorResultType()} if result_is_temporary else None


def _constant_emit(
    result: ValueRef,
    value: int | AttrProject | ValueTypeProject,
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
