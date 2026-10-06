# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

from loom.target.arch.spirv.features import feature_bits_value
from loom.target.arch.spirv.ordinary_vector import OrdinaryVectorType
from loom.target.arch.spirv.subgroup import (
    SPIRV_SUBGROUP_BALLOT_INSTRUCTION,
    SPIRV_SUBGROUP_BALLOT_PACKING_INSTRUCTIONS,
    SPIRV_SUBGROUP_U64_TYPE,
    SPIRV_SUBGROUP_V2U32_TYPE,
    SPIRV_SUBGROUP_V4U32_TYPE,
)


def test_ballot_uses_the_native_spirv_four_word_result() -> None:
    ballot = SPIRV_SUBGROUP_BALLOT_INSTRUCTION
    assert ballot.result_type == SPIRV_SUBGROUP_V4U32_TYPE
    assert ballot.result_type.suffix == "v4u32"
    assert ballot.operand_types[0].suffix == "bool"
    assert ballot.feature_bits == feature_bits_value(("group_non_uniform_ballot",))


def test_ballot_packing_preserves_the_low_64_lane_bits() -> None:
    extract, construct, bitcast = SPIRV_SUBGROUP_BALLOT_PACKING_INSTRUCTIONS
    assert extract.result_type.suffix == "u32"
    assert extract.operand_types == (SPIRV_SUBGROUP_V4U32_TYPE,)
    assert extract.component_index_maximum == 3
    assert construct.result_type == SPIRV_SUBGROUP_V2U32_TYPE
    assert construct.operand_types == (
        SPIRV_SUBGROUP_V2U32_TYPE.component_type,
        SPIRV_SUBGROUP_V2U32_TYPE.component_type,
    )
    assert bitcast.operand_types == (SPIRV_SUBGROUP_V2U32_TYPE,)
    assert bitcast.result_type == SPIRV_SUBGROUP_U64_TYPE
    assert isinstance(bitcast.operand_types[0], OrdinaryVectorType)
    assert (
        bitcast.operand_types[0].lane_count
        * bitcast.operand_types[0].component_type.bit_width
        == bitcast.result_type.bit_width
        == 64
    )


def test_ballot_native_types_remain_source_ineligible() -> None:
    assert not SPIRV_SUBGROUP_V4U32_TYPE.component_type.source_types
    assert not SPIRV_SUBGROUP_V2U32_TYPE.component_type.source_types
    assert not SPIRV_SUBGROUP_U64_TYPE.source_types
