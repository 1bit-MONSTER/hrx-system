# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""SPIR-V subgroup source-to-low contract rows."""

from __future__ import annotations

from loom.dialect.kernel import defs as kernel
from loom.target.arch.spirv.contracts.descriptor_rule import (
    IntegerRecipe,
    descriptor_feature_guards,
    emit_descriptor_op,
    logical_core_descriptor,
)
from loom.target.arch.spirv.ordinary_vector import (
    OrdinaryVectorComponentKind,
    OrdinaryVectorType,
)
from loom.target.arch.spirv.scalar_alu import (
    INTEGER_SCALAR_ALU_TYPE_PAIRS,
    IntegerAluTypePair,
)
from loom.target.arch.spirv.subgroup import (
    SPIRV_SUBGROUP_BALLOT_INSTRUCTION,
    SPIRV_SUBGROUP_BALLOT_PACKING_INSTRUCTIONS,
    SPIRV_SUBGROUP_REDUCTIONS,
    SPIRV_SUBGROUP_VOTE_INSTRUCTIONS,
    SpirvSubgroupReduction,
)
from loom.target.contracts import (
    AttrProject,
    ContractCase,
    DescriptorEmitForm,
    DescriptorResultType,
    DescriptorRule,
    EmitDescriptorOp,
    Guard,
    Scalar,
    ValueRef,
    Vector,
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


def _ballot_emit(mask_type: str, predicate: ValueRef) -> tuple[EmitDescriptorOp, ...]:
    native_mask = ValueRef.temporary("native_mask")
    low_word = ValueRef.temporary("low_word")
    emits = [
        emit_descriptor_op(
            descriptor=_BALLOT,
            operands={"predicate": predicate},
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


def _ballot_rule(
    mask_type: str, mask_bit_count: int, *, active_mask: bool = False
) -> DescriptorRule:
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
    predicate = ValueRef.operand("predicate")
    prefix = ()
    predicate_guards = (Guard.value_type("predicate", Scalar("i1")),)
    if active_mask:
        predicate = ValueRef.temporary("active")
        predicate_guards = ()
        constant = logical_core_descriptor("spirv.op_constant_true.bool")
        descriptors = (*descriptors, constant)
        prefix = (
            EmitDescriptorOp(
                descriptor=constant,
                results={"dst": predicate},
                result_types={"dst": DescriptorResultType()},
                form=DescriptorEmitForm.CONST,
            ),
        )
    return DescriptorRule(
        source_op=(
            kernel.kernel_subgroup_active_mask
            if active_mask
            else kernel.kernel_subgroup_vote_ballot
        ),
        descriptor=_BALLOT,
        guards=(
            *predicate_guards,
            Guard.value_type("mask", Scalar(mask_type)),
            Guard.target_subgroup_size_range(1, mask_bit_count),
            *descriptor_feature_guards(*descriptors),
        ),
        emit=(*prefix, *_ballot_emit(mask_type, predicate)),
    )


def _vote_rules() -> tuple[DescriptorRule, ...]:
    return tuple(
        DescriptorRule(
            source_op=source,
            descriptor=descriptor,
            guards=(
                Guard.value_type("predicate", Scalar("i1")),
                Guard.value_type("result", Scalar("i1")),
                *descriptor_feature_guards(descriptor),
            ),
            emit=(
                emit_descriptor_op(
                    descriptor=descriptor,
                    operands={"predicate": ValueRef.operand("predicate")},
                    results={"dst": ValueRef.result("result")},
                ),
            ),
        )
        for source, row in zip(
            (kernel.kernel_subgroup_vote_any, kernel.kernel_subgroup_vote_all),
            SPIRV_SUBGROUP_VOTE_INSTRUCTIONS,
            strict=True,
        )
        for descriptor in (logical_core_descriptor(row.key),)
    )


def _reduction_rule(
    row: SpirvSubgroupReduction,
    source_lanes: int,
    *,
    float_extremum: bool = False,
) -> DescriptorRule:
    instruction = row.instruction
    value_type = instruction.result_type
    component = (
        value_type.component_type
        if isinstance(value_type, OrdinaryVectorType)
        else value_type
    )
    source_type = row.source_type
    kinds = row.combining_kinds
    descriptor = logical_core_descriptor(instruction.key)
    cluster_guard = (
        Guard.target_contiguous_subgroup_cluster
        if instruction.clustered
        else Guard.target_whole_subgroup
    )("cluster_size", "cluster_stride")
    immediates = (
        {"cluster_size": AttrProject.direct("cluster_size")}
        if instruction.clustered
        else {}
    )
    recipe = IntegerRecipe(component.bit_width, max(1, source_lanes))
    value = ValueRef.operand("value")
    result = ValueRef.result("result")
    if float_extremum:
        source_type = f"f{component.bit_width}"
        minimum = kinds == ("minsi",)
        kinds = ("minnumf" if minimum else "maxnumf",)
        # A signed sortable key orders every finite value, infinity and signed
        # zero without any floating-point arithmetic. NaNs become an outlying
        # identity key, so numeric peers win and all-NaN groups stay quiet NaN.
        value = recipe.import_float_bits(value, source_type, component.bit_width)
        magnitude_mask = recipe.constant((1 << (component.bit_width - 1)) - 1)
        shift = recipe.constant(component.bit_width - 1)
        infinity = recipe.constant(
            {16: 0x7C00, 32: 0x7F800000, 64: 0x7FF0000000000000}[component.bit_width]
        )
        magnitude = recipe.binary("bitwise_and", value, magnitude_mask)
        is_nan = recipe.binary("s_greater_than", magnitude, infinity)
        sign = recipe.binary("shift_right_arithmetic", value, shift)
        ordered = recipe.binary(
            "bitwise_xor", value, recipe.binary("bitwise_and", sign, magnitude_mask)
        )
        identity = recipe.constant(
            (1 << (component.bit_width - 1)) - 1
            if minimum
            else -(1 << (component.bit_width - 1))
        )
        value = recipe.select(is_nan, identity, ordered)
        result = ValueRef.temporary("reduced_key")
    elif component.kind == OrdinaryVectorComponentKind.UNSIGNED_INTEGER:
        value = recipe.emit(
            f"bitcast.{recipe.integer_suffix}.{value_type.suffix}", {"input": value}
        )
        result = ValueRef.temporary("unsigned_result")
    recipe.emits.append(
        emit_descriptor_op(
            descriptor=descriptor,
            operands={"value": value},
            results={"dst": result},
            result_types={"dst": DescriptorResultType()},
            immediates=immediates,
        )
    )
    if float_extremum:
        sign = recipe.binary("shift_right_arithmetic", result, shift)
        bits = recipe.binary(
            "bitwise_xor", result, recipe.binary("bitwise_and", sign, magnitude_mask)
        )
        recipe.export_float_bits(bits, source_type, component.bit_width)
    elif component.kind == OrdinaryVectorComponentKind.UNSIGNED_INTEGER:
        recipe.emit(
            f"bitcast.{value_type.suffix}.{recipe.integer_suffix}",
            {"input": result},
            result=ValueRef.result("result"),
        )
    pattern = (
        Scalar(source_type)
        if source_lanes == 0
        else Vector(source_type, lanes=source_lanes)
    )
    return DescriptorRule(
        source_op=kernel.kernel_subgroup_reduce,
        descriptor=descriptor,
        guards=(
            Guard.enum_attr_in("kind", kinds),
            Guard.value_type("value", pattern),
            cluster_guard,
            *descriptor_feature_guards(*(emit.descriptor for emit in recipe.emits)),
        ),
        emit=tuple(recipe.emits),
        report_key="integer_float_extrema" if float_extremum else None,
    )


def _arithmetic_rules() -> tuple[DescriptorRule, ...]:
    rules = []
    for row in SPIRV_SUBGROUP_REDUCTIONS:
        value_type = row.instruction.result_type
        lane_counts = (
            (value_type.lane_count,)
            if isinstance(value_type, OrdinaryVectorType)
            else (0, 1)
        )
        for source_lanes in lane_counts:
            rules.append(_reduction_rule(row, source_lanes))
            if row.source_type in ("i16", "i32", "i64") and row.combining_kinds in (
                ("minsi",),
                ("maxsi",),
            ):
                rules.append(_reduction_rule(row, source_lanes, float_extremum=True))
    return tuple(rules)


SPIRV_SUBGROUP_CONTRACT_CASES: tuple[ContractCase, ...] = (
    _ballot_rule("i32", 32),
    _ballot_rule("i64", 64),
    _ballot_rule("i32", 32, active_mask=True),
    _ballot_rule("i64", 64, active_mask=True),
    *_vote_rules(),
    *_arithmetic_rules(),
)
