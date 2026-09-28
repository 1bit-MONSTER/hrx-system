# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

from loom.target.arch.spirv.contracts.subgroup import SPIRV_SUBGROUP_CONTRACT_CASES
from loom.target.contracts import DescriptorResultType, GuardKind, SourceValueKind


def test_ballot_contracts_cover_both_public_mask_widths() -> None:
    assert len(SPIRV_SUBGROUP_CONTRACT_CASES) == 2
    by_mask_width = {
        next(
            guard.maximum
            for guard in rule.guards
            if guard.kind is GuardKind.TARGET_SUBGROUP_SIZE_RANGE
        ): rule
        for rule in SPIRV_SUBGROUP_CONTRACT_CASES
    }
    assert set(by_mask_width) == {32, 64}
    for mask_bit_count, rule in by_mask_width.items():
        size_guard = next(
            guard
            for guard in rule.guards
            if guard.kind is GuardKind.TARGET_SUBGROUP_SIZE_RANGE
        )
        assert size_guard.minimum == 1
        assert size_guard.maximum == mask_bit_count
        assert any(
            guard.kind is GuardKind.DESCRIPTOR_AVAILABLE for guard in rule.guards
        )


def test_ballot_contract_keeps_native_packing_explicit() -> None:
    i32_rule, i64_rule = SPIRV_SUBGROUP_CONTRACT_CASES
    assert tuple(emit.descriptor.key for emit in i32_rule.emit) == (
        "spirv.op_group_non_uniform_ballot.v4u32",
        "spirv.op_composite_extract.v4u32.u32",
        "spirv.op_bitcast.u32.i32",
    )
    assert tuple(emit.descriptor.key for emit in i64_rule.emit) == (
        "spirv.op_group_non_uniform_ballot.v4u32",
        "spirv.op_composite_extract.v4u32.u32",
        "spirv.op_composite_extract.v4u32.u32",
        "spirv.op_composite_construct.v2u32",
        "spirv.op_bitcast.v2u32.u64",
        "spirv.op_bitcast.u64.i64",
    )
    assert i32_rule.emit[1].immediates == {"component_index": 0}
    assert i64_rule.emit[1].immediates == {"component_index": 0}
    assert i64_rule.emit[2].immediates == {"component_index": 1}
    for emit in (*i32_rule.emit[:-1], *i64_rule.emit[:-1]):
        for result_name, result_ref in emit.results.items():
            if result_ref.kind is SourceValueKind.TEMPORARY:
                assert isinstance(emit.result_types[result_name], DescriptorResultType)
