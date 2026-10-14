# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

from collections import Counter

from loom.target.arch.spirv.contracts.integer_bits import bit_count_rules
from loom.target.arch.spirv.descriptors import SPIRV_LOGICAL_CORE_DESCRIPTOR_SET
from loom.target.arch.spirv.features import feature_bits_value
from loom.target.contracts import DescriptorRule, GuardKind, ValueAliasRule


def test_bit_counts_cover_every_integer_width_and_native_shape() -> None:
    actual = Counter()
    for rule in bit_count_rules():
        rule.validate(SPIRV_LOGICAL_CORE_DESCRIPTOR_SET)
        pattern = next(
            guard.type_pattern
            for guard in rule.guards
            if guard.kind is GuardKind.VALUE_TYPE and guard.field == "input"
        )
        actual[
            (rule.source_op.name, pattern.kind, pattern.elements, pattern.lanes)
        ] += 1
    expected = Counter()
    for operation in ("ctlzi", "cttzi", "ctpopi"):
        for width in (1, 8, 16, 32, 64):
            element = (f"i{width}",)
            expected[(f"scalar.{operation}", "scalar", element, None)] = 1
            for lanes in (1, 2, 3, 4):
                expected[(f"vector.{operation}", "vector", element, lanes)] = 1
    assert actual == expected


def test_bit_count_recipes_use_only_the_authored_integer_capabilities() -> None:
    for rule in bit_count_rules():
        pattern = next(
            guard.type_pattern
            for guard in rule.guards
            if guard.kind is GuardKind.VALUE_TYPE and guard.field == "input"
        )
        element = pattern.elements[0]
        required = feature_bits_value(
            () if element in ("i1", "i32") else (f"int{element[1:]}",)
        )
        guarded = 0
        for guard in rule.guards:
            if guard.kind is GuardKind.DESCRIPTOR_AVAILABLE:
                for mask in guard.descriptor.feature_mask_words:
                    guarded |= mask
        assert guarded == required
        if isinstance(rule, ValueAliasRule):
            assert element == "i1" and rule.source_op.name.endswith("ctpopi")
            continue
        assert isinstance(rule, DescriptorRule)
        scans = [step for step in rule.emit if "find_umsb" in step.descriptor.key]
        if element != "i1" and rule.source_op.name.endswith("ctlzi"):
            assert len(scans) == 1
            assert scans[0].descriptor.key.endswith("i32")
        else:
            assert not scans
        if rule.source_op.name.endswith("ctpopi"):
            assert len(rule.emit) == 1
        elif element != "i1":
            assert rule.report_key == "integer_bit_count"
