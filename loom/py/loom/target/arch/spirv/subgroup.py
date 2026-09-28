# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Source-of-truth rows for SPIR-V subgroup operations."""

from __future__ import annotations

from dataclasses import dataclass

from loom.target.arch.spirv.features import feature_bits_value
from loom.target.arch.spirv.ordinary_vector import (
    BOOLEAN_ORDINARY_VECTOR_COMPONENT_TYPE,
    OrdinaryVectorComponentType,
    OrdinaryVectorInstruction,
    OrdinaryVectorType,
)
from loom.target.arch.spirv.ordinary_vector_integer import (
    ORDINARY_VECTOR_INTEGER_COMPONENT_TYPE_PAIRS,
)


def _unsigned_component(source_type: str) -> OrdinaryVectorComponentType:
    matches = tuple(
        pair.unsigned
        for pair in ORDINARY_VECTOR_INTEGER_COMPONENT_TYPE_PAIRS
        if pair.source_type == source_type
    )
    if len(matches) != 1:
        raise ValueError(f"expected one unsigned SPIR-V component for {source_type!r}")
    return matches[0]


SPIRV_SUBGROUP_U32_TYPE = _unsigned_component("i32")
SPIRV_SUBGROUP_U64_TYPE = _unsigned_component("i64")
SPIRV_SUBGROUP_V2U32_TYPE = OrdinaryVectorType(SPIRV_SUBGROUP_U32_TYPE, 2)
SPIRV_SUBGROUP_V4U32_TYPE = OrdinaryVectorType(SPIRV_SUBGROUP_U32_TYPE, 4)


@dataclass(frozen=True, slots=True)
class SpirvSubgroupInstruction:
    """One native SPIR-V subgroup instruction and its value contract."""

    key: str
    mnemonic: str
    opcode: str
    packet_form: str
    result_type: OrdinaryVectorType
    operand_names: tuple[str, ...]
    operand_types: tuple[OrdinaryVectorComponentType, ...]
    feature_atoms: tuple[str, ...]

    def __post_init__(self) -> None:
        if len(self.operand_names) != len(self.operand_types):
            raise ValueError(
                f"{self.key}: operand names and types must have equal length"
            )

    @property
    def feature_bits(self) -> int:
        bits = self.result_type.feature_bits | feature_bits_value(self.feature_atoms)
        for operand_type in self.operand_types:
            bits |= operand_type.feature_bits
        return bits


SPIRV_SUBGROUP_BALLOT_INSTRUCTION = SpirvSubgroupInstruction(
    key="spirv.op_group_non_uniform_ballot.v4u32",
    mnemonic="OpGroupNonUniformBallot.v4u32",
    opcode="LOOM_SPIRV_OP_GROUP_NON_UNIFORM_BALLOT",
    packet_form="LOOM_SPIRV_PACKET_FORM_GROUP_NON_UNIFORM_BALLOT",
    result_type=SPIRV_SUBGROUP_V4U32_TYPE,
    operand_names=("predicate",),
    operand_types=(BOOLEAN_ORDINARY_VECTOR_COMPONENT_TYPE,),
    feature_atoms=("group_non_uniform_ballot",),
)


SPIRV_SUBGROUP_BALLOT_PACKING_INSTRUCTIONS = (
    OrdinaryVectorInstruction(
        key="spirv.op_composite_extract.v4u32.u32",
        mnemonic="OpCompositeExtract.v4u32.u32",
        opcode="LOOM_SPIRV_OP_COMPOSITE_EXTRACT",
        packet_form="LOOM_SPIRV_PACKET_FORM_COMPOSITE_EXTRACT",
        result_type=SPIRV_SUBGROUP_U32_TYPE,
        operand_names=("composite",),
        operand_types=(SPIRV_SUBGROUP_V4U32_TYPE,),
        component_index_maximum=3,
    ),
    OrdinaryVectorInstruction(
        key="spirv.op_composite_construct.v2u32",
        mnemonic="OpCompositeConstruct.v2u32",
        opcode="LOOM_SPIRV_OP_COMPOSITE_CONSTRUCT",
        packet_form="LOOM_SPIRV_PACKET_FORM_COMPOSITE_CONSTRUCT",
        result_type=SPIRV_SUBGROUP_V2U32_TYPE,
        operand_names=("component0", "component1"),
        operand_types=(SPIRV_SUBGROUP_U32_TYPE, SPIRV_SUBGROUP_U32_TYPE),
    ),
    OrdinaryVectorInstruction(
        key="spirv.op_bitcast.v2u32.u64",
        mnemonic="OpBitcast.v2u32.u64",
        opcode="LOOM_SPIRV_OP_BITCAST",
        packet_form="LOOM_SPIRV_PACKET_FORM_UNARY_TYPED",
        result_type=SPIRV_SUBGROUP_U64_TYPE,
        operand_names=("input",),
        operand_types=(SPIRV_SUBGROUP_V2U32_TYPE,),
    ),
)
