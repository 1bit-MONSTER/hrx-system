# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""SPIR-V subgroup source-to-low contract rows."""

from __future__ import annotations

from loom.dialect.kernel import defs as kernel
from loom.target.arch.spirv.contracts.descriptor_rule import (
    descriptor_feature_guards,
    emit_descriptor_op,
    logical_core_descriptor,
)
from loom.target.arch.spirv.scalar_alu import (
    INTEGER_SCALAR_ALU_TYPE_PAIRS,
    IntegerAluTypePair,
)
from loom.target.arch.spirv.subgroup import (
    SPIRV_SUBGROUP_BALLOT_INSTRUCTION,
    SPIRV_SUBGROUP_BALLOT_PACKING_INSTRUCTIONS,
)
from loom.target.contracts import (
    ContractCase,
    DescriptorResultType,
    DescriptorRule,
    EmitDescriptorOp,
    Guard,
    Scalar,
    ValueRef,
)
from loom.target.low_descriptors import Descriptor


def _integer_pair(source_type: str) -> IntegerAluTypePair:
    matches = tuple(
        pair
        for pair in INTEGER_SCALAR_ALU_TYPE_PAIRS
        if pair.source_type == source_type
    )
    if len(matches) != 1:
        raise ValueError(f"expected one SPIR-V integer pair for {source_type!r}")
    return matches[0]


_I32_PAIR = _integer_pair("i32")
_I64_PAIR = _integer_pair("i64")
_BALLOT = logical_core_descriptor(SPIRV_SUBGROUP_BALLOT_INSTRUCTION.key)
_EXTRACT_WORD = logical_core_descriptor(
    SPIRV_SUBGROUP_BALLOT_PACKING_INSTRUCTIONS[0].key
)
_CONSTRUCT_WORD_PAIR = logical_core_descriptor(
    SPIRV_SUBGROUP_BALLOT_PACKING_INSTRUCTIONS[1].key
)
_BITCAST_WORD_PAIR_TO_U64 = logical_core_descriptor(
    SPIRV_SUBGROUP_BALLOT_PACKING_INSTRUCTIONS[2].key
)


def _integer_view_descriptor(pair: IntegerAluTypePair) -> Descriptor:
    return logical_core_descriptor(
        f"spirv.op_bitcast.{pair.unsigned.suffix}.{pair.signed.suffix}"
    )


def _ballot_emit(mask_type: str) -> tuple[EmitDescriptorOp, ...]:
    native_mask = ValueRef.temporary("native_mask")
    low_word = ValueRef.temporary("low_word")
    emits = [
        emit_descriptor_op(
            descriptor=_BALLOT,
            operands={"predicate": ValueRef.operand("predicate")},
            results={"dst": native_mask},
            result_types={"dst": DescriptorResultType()},
        ),
        emit_descriptor_op(
            descriptor=_EXTRACT_WORD,
            operands={"composite": native_mask},
            results={"dst": low_word},
            result_types={"dst": DescriptorResultType()},
            immediates={"component_index": 0},
        ),
    ]
    if mask_type == "i32":
        emits.append(
            emit_descriptor_op(
                descriptor=_integer_view_descriptor(_I32_PAIR),
                operands={"input": low_word},
                results={"dst": ValueRef.result("mask")},
            )
        )
        return tuple(emits)

    high_word = ValueRef.temporary("high_word")
    packed_words = ValueRef.temporary("packed_words")
    unsigned_mask = ValueRef.temporary("unsigned_mask")
    emits.extend(
        (
            emit_descriptor_op(
                descriptor=_EXTRACT_WORD,
                operands={"composite": native_mask},
                results={"dst": high_word},
                result_types={"dst": DescriptorResultType()},
                immediates={"component_index": 1},
            ),
            emit_descriptor_op(
                descriptor=_CONSTRUCT_WORD_PAIR,
                operands={"component0": low_word, "component1": high_word},
                results={"dst": packed_words},
                result_types={"dst": DescriptorResultType()},
            ),
            emit_descriptor_op(
                descriptor=_BITCAST_WORD_PAIR_TO_U64,
                operands={"input": packed_words},
                results={"dst": unsigned_mask},
                result_types={"dst": DescriptorResultType()},
            ),
            emit_descriptor_op(
                descriptor=_integer_view_descriptor(_I64_PAIR),
                operands={"input": unsigned_mask},
                results={"dst": ValueRef.result("mask")},
            ),
        )
    )
    return tuple(emits)


def _ballot_rule(mask_type: str, mask_bit_count: int) -> DescriptorRule:
    descriptors = (
        _BALLOT,
        _EXTRACT_WORD,
        _integer_view_descriptor(_I32_PAIR),
    )
    if mask_type == "i64":
        descriptors = (
            *descriptors,
            _CONSTRUCT_WORD_PAIR,
            _BITCAST_WORD_PAIR_TO_U64,
            _integer_view_descriptor(_I64_PAIR),
        )
    return DescriptorRule(
        source_op=kernel.kernel_subgroup_vote_ballot,
        descriptor=_BALLOT,
        guards=(
            Guard.value_type("predicate", Scalar("i1")),
            Guard.value_type("mask", Scalar(mask_type)),
            Guard.target_subgroup_size_range(1, mask_bit_count),
            *descriptor_feature_guards(*descriptors),
        ),
        emit=_ballot_emit(mask_type),
    )


SPIRV_SUBGROUP_CONTRACT_CASES: tuple[ContractCase, ...] = (
    _ballot_rule("i32", 32),
    _ballot_rule("i64", 64),
)
