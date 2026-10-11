# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Scalar numeric conversion semantics lifted to native SPIR-V vectors."""

from __future__ import annotations

from dataclasses import dataclass

from loom.target.arch.spirv.ordinary_vector import (
    NATIVE_ORDINARY_VECTOR_LANE_COUNTS,
    ORDINARY_VECTOR_TYPES,
    OrdinaryVectorInstruction,
    OrdinaryVectorType,
)
from loom.target.arch.spirv.ordinary_vector_integer import (
    ORDINARY_VECTOR_INTEGER_TYPE_PAIRS,
)
from loom.target.arch.spirv.scalar_alu import (
    FLOAT_SCALAR_ALU_TYPES,
    SIGNED_INTEGER_SCALAR_ALU_TYPES,
    ScalarAluType,
)
from loom.target.arch.spirv.scalar_conversion import (
    LOW_SCALAR_CONVERSIONS,
    ScalarConversion,
)


@dataclass(frozen=True, slots=True)
class OrdinaryVectorConversion:
    """One scalar numeric conversion lifted to a native vector type pair."""

    # Owning scalar opcode, interpretation and feature contract.
    scalar_conversion: ScalarConversion
    # Native input shape with the opcode's physical component signedness.
    source_type: OrdinaryVectorType
    # Native output shape with the opcode's physical component signedness.
    result_type: OrdinaryVectorType

    def __post_init__(self) -> None:
        if self.source_type.lane_count != self.result_type.lane_count:
            raise ValueError("ordinary-vector conversion lane counts must match")
        for scalar_type, vector_type in (
            (self.scalar_conversion.source_type, self.source_type),
            (self.scalar_conversion.result_type, self.result_type),
        ):
            component_type = vector_type.component_type
            if component_type.suffix != scalar_type.suffix:
                raise ValueError("scalar and vector conversion types must correspond")
            if component_type.scalar_enum != scalar_type.scalar_enum:
                raise ValueError("scalar and vector conversion encodings must match")
            if component_type.feature_atoms != scalar_type.feature_atoms:
                raise ValueError("scalar and vector conversion features must match")

    @property
    def instruction(self) -> OrdinaryVectorInstruction:
        scalar_conversion = self.scalar_conversion
        return OrdinaryVectorInstruction(
            key=(
                f"spirv.op_{scalar_conversion.descriptor_suffix}."
                f"{self.source_type.suffix}.{self.result_type.suffix}"
            ),
            mnemonic=(
                f"{scalar_conversion.mnemonic}."
                f"{self.source_type.suffix}.{self.result_type.suffix}"
            ),
            opcode=scalar_conversion.opcode,
            packet_form="LOOM_SPIRV_PACKET_FORM_UNARY_TYPED",
            result_type=self.result_type,
            operand_names=("input",),
            operand_types=(self.source_type,),
            feature_atoms=scalar_conversion.feature_atoms,
        )


_CONVERSION_VECTOR_TYPES = (
    *ORDINARY_VECTOR_TYPES,
    *(type_pair.unsigned for type_pair in ORDINARY_VECTOR_INTEGER_TYPE_PAIRS),
)
_VECTOR_TYPES_BY_SCALAR_SUFFIX_AND_LANE = {
    (vector_type.component_type.suffix, vector_type.lane_count): vector_type
    for vector_type in _CONVERSION_VECTOR_TYPES
}
if len(_VECTOR_TYPES_BY_SCALAR_SUFFIX_AND_LANE) != len(_CONVERSION_VECTOR_TYPES):
    raise ValueError("vector scalar suffix and lane pairs must be unique")


def _vector_type(
    scalar_type: ScalarAluType,
    lane_count: int,
) -> OrdinaryVectorType:
    vector_type = _VECTOR_TYPES_BY_SCALAR_SUFFIX_AND_LANE.get(
        (scalar_type.suffix, lane_count)
    )
    if vector_type is None:
        raise ValueError(
            f"{scalar_type.suffix}: missing v{lane_count} numeric vector type"
        )
    return vector_type


# BF16/FP8 payloads and predicates have different representation contracts.
# Bit-layout conversions are owned by ordinary_vector_bit_layout.
_NUMERIC_SOURCE_TYPES = frozenset(
    scalar.source_type
    for scalar in (*SIGNED_INTEGER_SCALAR_ALU_TYPES, *FLOAT_SCALAR_ALU_TYPES)
)
NATIVE_VECTOR_SCALAR_CONVERSIONS = tuple(
    conversion
    for conversion in LOW_SCALAR_CONVERSIONS
    if conversion.source_op_key != "bitcast"
    and conversion.source_type.source_type in _NUMERIC_SOURCE_TYPES
    and conversion.result_type.source_type in _NUMERIC_SOURCE_TYPES
)

ORDINARY_VECTOR_CONVERSIONS = tuple(
    OrdinaryVectorConversion(
        scalar_conversion=conversion,
        source_type=_vector_type(conversion.source_type, lane_count),
        result_type=_vector_type(conversion.result_type, lane_count),
    )
    for conversion in NATIVE_VECTOR_SCALAR_CONVERSIONS
    for lane_count in NATIVE_ORDINARY_VECTOR_LANE_COUNTS
)

ORDINARY_VECTOR_CONVERSION_INSTRUCTIONS = tuple(
    conversion.instruction for conversion in ORDINARY_VECTOR_CONVERSIONS
)
