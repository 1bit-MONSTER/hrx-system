# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

from loom.dialect.kernel import defs as kernel
from loom.target.arch.spirv.contracts.subgroup import SPIRV_SUBGROUP_CONTRACT_CASES
from loom.target.arch.spirv.descriptors import SPIRV_LOGICAL_CORE_DESCRIPTOR_SET
from loom.target.arch.spirv.features import feature_bits_value
from loom.target.contracts import DescriptorResultType, GuardKind, SourceValueKind


def _ballot_rules():
    return tuple(
        rule
        for rule in SPIRV_SUBGROUP_CONTRACT_CASES
        if rule.source_op is kernel.kernel_subgroup_vote_ballot
    )


def test_ballot_contracts_cover_both_public_mask_widths() -> None:
    assert len(_ballot_rules()) == 2
    by_mask_width = {
        next(
            guard.maximum
            for guard in rule.guards
            if guard.kind is GuardKind.TARGET_SUBGROUP_SIZE_RANGE
        ): rule
        for rule in _ballot_rules()
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
    i32_rule, i64_rule = _ballot_rules()
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


def test_active_masks_reuse_guarded_ballot_packing_with_a_true_predicate() -> None:
    masks = tuple(
        rule
        for rule in SPIRV_SUBGROUP_CONTRACT_CASES
        if rule.source_op is kernel.kernel_subgroup_active_mask
    )
    assert len(masks) == 2
    for mask, ballot in zip(masks, _ballot_rules(), strict=True):
        assert mask.emit[0].descriptor.key == "spirv.op_constant_true.bool"
        assert mask.emit[1].operands["predicate"] == mask.emit[0].results["dst"]
        assert tuple(emit.descriptor for emit in mask.emit[1:]) == tuple(
            emit.descriptor for emit in ballot.emit
        )
        assert tuple(
            guard
            for guard in mask.guards
            if guard.kind is GuardKind.TARGET_SUBGROUP_SIZE_RANGE
        ) == tuple(
            guard
            for guard in ballot.guards
            if guard.kind is GuardKind.TARGET_SUBGROUP_SIZE_RANGE
        )


def test_boolean_votes_are_single_native_operations() -> None:
    for operation, suffix in (
        (kernel.kernel_subgroup_vote_any, "any"),
        (kernel.kernel_subgroup_vote_all, "all"),
    ):
        rules = tuple(
            rule
            for rule in SPIRV_SUBGROUP_CONTRACT_CASES
            if rule.source_op is operation
        )
        assert len(rules) == 1
        rule = rules[0]
        assert len(rule.emit) == 1
        assert rule.descriptor.key == f"spirv.op_group_non_uniform_{suffix}.bool"
        assert any(
            guard.kind is GuardKind.DESCRIPTOR_AVAILABLE for guard in rule.guards
        )


def test_reduction_family_covers_native_types_shapes_and_modes() -> None:
    actual = set()
    for rule in SPIRV_SUBGROUP_CONTRACT_CASES:
        if rule.source_op is not kernel.kernel_subgroup_reduce:
            continue
        rule.validate(SPIRV_LOGICAL_CORE_DESCRIPTOR_SET)
        pattern = next(
            guard.type_pattern
            for guard in rule.guards
            if guard.kind is GuardKind.VALUE_TYPE
        )
        kinds = next(
            guard.enum_keywords
            for guard in rule.guards
            if guard.kind is GuardKind.ENUM_ATTR_IN
        )
        mode = next(
            guard.kind
            for guard in rule.guards
            if guard.kind
            in (
                GuardKind.TARGET_WHOLE_SUBGROUP,
                GuardKind.TARGET_CONTIGUOUS_SUBGROUP_CLUSTER,
            )
        )
        for kind in kinds:
            cell = (pattern.elements[0], pattern.lanes, kind, mode)
            assert cell not in actual, cell
            actual.add(cell)
        group_emits = [
            emit
            for emit in rule.emit
            if emit.descriptor.key.startswith("spirv.op_group_non_uniform_")
        ]
        assert len(group_emits) == 1
        clustered = mode is GuardKind.TARGET_CONTIGUOUS_SUBGROUP_CLUSTER
        assert bool(group_emits[0].immediates) == clustered
        if clustered:
            assert group_emits[0].descriptor.immediates[0].unsigned_max == (1 << 32) - 1
        if pattern.elements[0] == "i1" or kinds in (("addf",), ("mulf",)):
            assert len(rule.emit) == 1
    expected = {
        (element, lanes, kind, mode)
        for elements, kinds in (
            (
                ("i1", "i8", "i16", "i32", "i64"),
                (
                    "addi",
                    "muli",
                    "minsi",
                    "maxsi",
                    "minui",
                    "maxui",
                    "andi",
                    "ori",
                    "xori",
                ),
            ),
            (("f16", "f32", "f64"), ("addf", "mulf", "minnumf", "maxnumf")),
        )
        for element in elements
        for lanes in (None, 1, 2, 3, 4)
        for kind in kinds
        for mode in (
            GuardKind.TARGET_WHOLE_SUBGROUP,
            GuardKind.TARGET_CONTIGUOUS_SUBGROUP_CLUSTER,
        )
    }
    assert actual == expected


def test_reduction_types_require_logical_device_admission() -> None:
    extended = feature_bits_value(("subgroup_extended_types",))
    for rule in SPIRV_SUBGROUP_CONTRACT_CASES:
        if rule.source_op is not kernel.kernel_subgroup_reduce:
            continue
        pattern = next(
            guard.type_pattern
            for guard in rule.guards
            if guard.kind is GuardKind.VALUE_TYPE
        )
        requires_extended = any(
            mask & extended
            for guard in rule.guards
            if guard.kind is GuardKind.DESCRIPTOR_AVAILABLE
            for mask in guard.descriptor.feature_mask_words
        )
        assert requires_extended == (
            pattern.elements[0] in ("i8", "i16", "i64", "f16")
            or (pattern.elements[0] == "f64" and bool(rule.report_key))
        )
        if rule.report_key:
            assert rule.report_key == "integer_float_extrema"
            assert len(rule.emit) <= 22
            assert all(
                not emit.descriptor.key.startswith("spirv.op_f") for emit in rule.emit
            )
