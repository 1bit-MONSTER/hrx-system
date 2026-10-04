# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

from loom.dialect.scalar import conversion
from loom.ir import ScalarType
from loom.scalar_type import ScalarTypeKind, scalar_type_name
from loom.target.arch.spirv.contracts.descriptor_rule import (
    descriptor_feature_guards,
    logical_core_descriptor,
)
from loom.target.arch.spirv.contracts.logical_core import (
    SPIRV_LOGICAL_CORE_CONTRACT_FRAGMENT,
)
from loom.target.contracts import DescriptorRule, GuardKind

_FLOAT_KINDS = frozenset(
    {
        ScalarTypeKind.F8E4M3,
        ScalarTypeKind.F8E5M2,
        ScalarTypeKind.F16,
        ScalarTypeKind.BF16,
        ScalarTypeKind.F32,
        ScalarTypeKind.F64,
    }
)
_NON_FLOAT_KINDS = frozenset(
    {
        ScalarTypeKind.INDEX,
        ScalarTypeKind.OFFSET,
        ScalarTypeKind.I1,
        ScalarTypeKind.I8,
        ScalarTypeKind.I16,
        ScalarTypeKind.I32,
        ScalarTypeKind.I64,
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


def _expected_narrowing_pairs() -> frozenset[tuple[str, str]]:
    return frozenset(
        (scalar_type_name(source), scalar_type_name(result))
        for source in _FLOAT_KINDS
        for result in _FLOAT_KINDS
        if ScalarType(source).bitwidth > ScalarType(result).bitwidth
    )


def test_scalar_float_narrowing_rules_cover_the_complete_type_matrix() -> None:
    assert _FLOAT_KINDS | _NON_FLOAT_KINDS == frozenset(ScalarTypeKind)
    assert _FLOAT_KINDS.isdisjoint(_NON_FLOAT_KINDS)

    expected_pairs = _expected_narrowing_pairs()
    actual_pairs = {
        _narrowing_pair(rule)
        for rule in SPIRV_LOGICAL_CORE_CONTRACT_FRAGMENT.cases
        if isinstance(rule, DescriptorRule)
        and rule.source_op is conversion.scalar_fptrunc
    }

    assert len(expected_pairs) == 13
    assert actual_pairs == expected_pairs


def test_generic_narrowing_recipes_cover_every_non_native_pair() -> None:
    native_pairs = frozenset(
        {
            ("f64", "f32"),
            ("f64", "f16"),
            ("f32", "f16"),
        }
    )
    exact_recipe_pairs = {
        _narrowing_pair(rule)
        for rule in SPIRV_LOGICAL_CORE_CONTRACT_FRAGMENT.cases
        if isinstance(rule, DescriptorRule)
        and rule.source_op is conversion.scalar_fptrunc
        and rule.report_key.startswith("exact_")
    }

    assert exact_recipe_pairs == _expected_narrowing_pairs() - native_pairs


def test_descriptor_feature_guards_deduplicate_only_identical_descriptors() -> None:
    f64_constant = logical_core_descriptor("spirv.op_constant.f64")
    f64_add = logical_core_descriptor("spirv.op_fadd.f64")
    featureless_i32_constant = logical_core_descriptor("spirv.op_constant.i32")

    guards = descriptor_feature_guards(
        f64_constant,
        f64_constant,
        f64_add,
        featureless_i32_constant,
    )

    assert tuple(guard.descriptor for guard in guards) == (f64_constant, f64_add)
