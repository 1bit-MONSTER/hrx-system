# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

import pytest

from loom.dialect.scalar import conversion
from loom.target.arch.x86.contracts.float_narrowing import (
    x86_float_narrowing_rules,
)
from loom.target.arch.x86.descriptors import (
    X86_AVX2_DESCRIPTOR_SET,
    X86_AVX512_CORE_DESCRIPTOR_SET,
)
from loom.target.contracts import (
    DescriptorEmitForm,
    DescriptorRule,
    GuardKind,
    SourceValueKind,
    descriptor_by_key,
)
from loom.target.low_descriptors import DescriptorSet

_EXPECTED_PAIRS = frozenset(
    {
        ("bf16", "f8E4M3"),
        ("bf16", "f8E5M2"),
        ("f16", "f8E4M3"),
        ("f16", "f8E5M2"),
        ("f32", "bf16"),
        ("f32", "f16"),
        ("f32", "f8E4M3"),
        ("f32", "f8E5M2"),
    }
)


def _narrowing_pair(rule: DescriptorRule) -> tuple[str, str]:
    type_guards = {
        guard.field: guard.type_pattern
        for guard in rule.guards
        if guard.kind is GuardKind.VALUE_TYPE
    }
    input_type = type_guards["input"]
    result_type = type_guards["result"]
    assert input_type is not None and input_type.kind == "scalar"
    assert result_type is not None and result_type.kind == "scalar"
    assert len(input_type.elements) == 1
    assert len(result_type.elements) == 1
    return input_type.elements[0], result_type.elements[0]


def _rules(descriptor_set: DescriptorSet) -> tuple[DescriptorRule, ...]:
    return x86_float_narrowing_rules(lambda key: descriptor_by_key(descriptor_set, key))


@pytest.mark.parametrize(
    "descriptor_set",
    [X86_AVX2_DESCRIPTOR_SET, X86_AVX512_CORE_DESCRIPTOR_SET],
)
def test_rules_cover_the_complete_x86_carrier_matrix(
    descriptor_set: DescriptorSet,
) -> None:
    rules = _rules(descriptor_set)

    assert all(rule.source_op is conversion.scalar_fptrunc for rule in rules)
    assert {_narrowing_pair(rule) for rule in rules} == _EXPECTED_PAIRS


@pytest.mark.parametrize(
    "descriptor_set",
    [X86_AVX2_DESCRIPTOR_SET, X86_AVX512_CORE_DESCRIPTOR_SET],
)
def test_rules_fold_static_literals_and_copy_dynamic_shift_counts(
    descriptor_set: DescriptorSet,
) -> None:
    rules = _rules(descriptor_set)
    immediate_keys = {
        emit.descriptor.key
        for rule in rules
        for emit in rule.emit
        if ".imm.gpr32" in emit.descriptor.key
    }

    assert immediate_keys == {
        "x86.scalar.add.imm.gpr32",
        "x86.scalar.and.imm.gpr32",
        "x86.scalar.cmp.sge.imm.gpr32",
        "x86.scalar.cmp.sgt.imm.gpr32",
        "x86.scalar.cmp.slt.imm.gpr32",
        "x86.scalar.or.imm.gpr32",
        "x86.scalar.shr.imm.gpr32",
        "x86.scalar.sub.imm.gpr32",
    }

    for rule in rules:
        constant_temporaries = {
            result.field
            for emit in rule.emit
            if emit.form is DescriptorEmitForm.CONST
            for result in emit.results.values()
            if result.kind is SourceValueKind.TEMPORARY
        }
        for emit in rule.emit:
            if emit.descriptor.key not in (
                "x86.scalar.shl.cl.gpr32",
                "x86.scalar.shr.cl.gpr32",
            ):
                continue
            assert emit.copy_operands == ("rhs",)
            shift = emit.operands["rhs"]
            assert shift.kind is SourceValueKind.TEMPORARY
            assert shift.field not in constant_temporaries
