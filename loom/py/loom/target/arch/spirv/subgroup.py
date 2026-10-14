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
    NATIVE_ORDINARY_VECTOR_LANE_COUNTS,
    ORDINARY_VECTOR_COMPONENT_TYPES,
    OrdinaryVectorComponentKind,
    OrdinaryVectorComponentType,
    OrdinaryVectorInstruction,
    OrdinaryVectorInstructionType,
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

    # Stable descriptor key including mode and native value shape.
    key: str
    # Human-readable Low assembly spelling.
    mnemonic: str
    # Native SPIR-V opcode constant.
    opcode: str
    # Shared binary operand layout selected by the emitter.
    packet_form: str
    # Native result component or vector type.
    result_type: OrdinaryVectorInstructionType
    # Named SSA operands in binary order.
    operand_names: tuple[str, ...]
    # Native types corresponding to operand_names.
    operand_types: tuple[OrdinaryVectorInstructionType, ...]
    # Required capabilities in addition to each value type's requirements.
    feature_atoms: tuple[str, ...]
    # SPIR-V arithmetic mode, absent for predicates and ballots.
    group_operation: str | None = None

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

    @property
    def clustered(self) -> bool:
        return self.group_operation == "LOOM_SPIRV_GROUP_OPERATION_CLUSTERED_REDUCE"


SPIRV_SUBGROUP_BALLOT_INSTRUCTION = SpirvSubgroupInstruction(
    key="spirv.op_group_non_uniform_ballot.v4u32",
    mnemonic="OpGroupNonUniformBallot.v4u32",
    opcode="LOOM_SPIRV_OP_GROUP_NON_UNIFORM_BALLOT",
    packet_form="LOOM_SPIRV_PACKET_FORM_GROUP_NON_UNIFORM_UNARY",
    result_type=SPIRV_SUBGROUP_V4U32_TYPE,
    operand_names=("predicate",),
    operand_types=(BOOLEAN_ORDINARY_VECTOR_COMPONENT_TYPE,),
    feature_atoms=("group_non_uniform_ballot",),
)

SPIRV_SUBGROUP_VOTE_INSTRUCTIONS = tuple(
    SpirvSubgroupInstruction(
        key=f"spirv.op_group_non_uniform_{name}.bool",
        mnemonic=f"OpGroupNonUniform{mnemonic}.bool",
        opcode=f"LOOM_SPIRV_OP_GROUP_NON_UNIFORM_{name.upper()}",
        packet_form="LOOM_SPIRV_PACKET_FORM_GROUP_NON_UNIFORM_UNARY",
        result_type=BOOLEAN_ORDINARY_VECTOR_COMPONENT_TYPE,
        operand_names=("predicate",),
        operand_types=(BOOLEAN_ORDINARY_VECTOR_COMPONENT_TYPE,),
        feature_atoms=("group_non_uniform_vote",),
    )
    for name, mnemonic in (("any", "Any"), ("all", "All"))
)


@dataclass(frozen=True, slots=True)
class SpirvSubgroupReduction:
    """Source combining kinds sharing one native typed reduction."""

    # Authored element type; unsigned target views retain their signed source.
    source_type: str
    # Source operations whose bit semantics equal the native instruction.
    combining_kinds: tuple[str, ...]
    # Native instruction, including its group operation and value type.
    instruction: SpirvSubgroupInstruction


def _reduction_operations(component: OrdinaryVectorComponentType):
    if component.kind == OrdinaryVectorComponentKind.BOOLEAN:
        # I1 arithmetic wraps modulo two; signed true represents -1.
        return (
            (
                ("muli", "maxsi", "minui", "andi"),
                "logical_and",
                "LogicalAnd",
                "LOGICAL_AND",
                component,
            ),
            (
                ("minsi", "maxui", "ori"),
                "logical_or",
                "LogicalOr",
                "LOGICAL_OR",
                component,
            ),
            (("addi", "xori"), "logical_xor", "LogicalXor", "LOGICAL_XOR", component),
        )
    if component.kind == OrdinaryVectorComponentKind.SIGNED_INTEGER:
        unsigned = _unsigned_component(component.source_types[0])
        return (
            (("addi",), "iadd", "IAdd", "I_ADD", component),
            (("muli",), "imul", "IMul", "I_MUL", component),
            (("minsi",), "smin", "SMin", "S_MIN", component),
            (("maxsi",), "smax", "SMax", "S_MAX", component),
            (("minui",), "umin", "UMin", "U_MIN", unsigned),
            (("maxui",), "umax", "UMax", "U_MAX", unsigned),
            (("andi",), "bitwise_and", "BitwiseAnd", "BITWISE_AND", component),
            (("ori",), "bitwise_or", "BitwiseOr", "BITWISE_OR", component),
            (("xori",), "bitwise_xor", "BitwiseXor", "BITWISE_XOR", component),
        )
    if component.source_types[0] in ("f16", "f32", "f64"):
        return (
            (("addf",), "fadd", "FAdd", "F_ADD", component),
            (("mulf",), "fmul", "FMul", "F_MUL", component),
        )
    return ()


def _reduction_rows() -> tuple[SpirvSubgroupReduction, ...]:
    rows = []
    for component in ORDINARY_VECTOR_COMPONENT_TYPES:
        extended = (
            component.kind == OrdinaryVectorComponentKind.SIGNED_INTEGER
            and component.bit_width != 32
        ) or component.source_types == ("f16",)
        for kinds, key, mnemonic, opcode, native_component in _reduction_operations(
            component
        ):
            for lane_count in (1, *NATIVE_ORDINARY_VECTOR_LANE_COUNTS):
                value_type = (
                    native_component
                    if lane_count == 1
                    else OrdinaryVectorType(native_component, lane_count)
                )
                rows.extend(
                    SpirvSubgroupReduction(
                        source_type=component.source_types[0],
                        combining_kinds=kinds,
                        instruction=SpirvSubgroupInstruction(
                            key=f"spirv.op_group_non_uniform_{key}.{mode}.{value_type.suffix}",
                            mnemonic=f"OpGroupNonUniform{mnemonic}.{mode}.{value_type.suffix}",
                            opcode=f"LOOM_SPIRV_OP_GROUP_NON_UNIFORM_{opcode}",
                            packet_form="LOOM_SPIRV_PACKET_FORM_GROUP_NON_UNIFORM_ARITHMETIC",
                            result_type=value_type,
                            operand_names=("value",),
                            operand_types=(value_type,),
                            feature_atoms=(
                                "group_non_uniform_arithmetic"
                                if mode == "reduce"
                                else "group_non_uniform_clustered",
                                *(("subgroup_extended_types",) if extended else ()),
                            ),
                            group_operation=f"LOOM_SPIRV_GROUP_OPERATION_{mode.upper()}",
                        ),
                    )
                    for mode in ("reduce", "clustered_reduce")
                )
    return tuple(rows)


SPIRV_SUBGROUP_REDUCTIONS = _reduction_rows()

SPIRV_SUBGROUP_INSTRUCTIONS = (
    SPIRV_SUBGROUP_BALLOT_INSTRUCTION,
    *SPIRV_SUBGROUP_VOTE_INSTRUCTIONS,
    *(row.instruction for row in SPIRV_SUBGROUP_REDUCTIONS),
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
