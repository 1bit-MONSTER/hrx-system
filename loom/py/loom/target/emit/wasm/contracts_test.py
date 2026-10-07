# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

from loom.dialect.index import defs as index
from loom.dialect.scalar import bitwise, conversion
from loom.ir import ScalarType
from loom.scalar_type import ScalarTypeKind, scalar_type_name
from loom.target.contracts import DescriptorRule, GuardKind
from loom.target.emit.wasm.contracts import WASM_CORE_SIMD128_CONTRACT_FRAGMENT

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
        for rule in WASM_CORE_SIMD128_CONTRACT_FRAGMENT.cases
        if isinstance(rule, DescriptorRule)
        and rule.source_op is conversion.scalar_fptrunc
    }

    assert len(expected_pairs) == 13
    assert actual_pairs == expected_pairs


def test_exact_recipes_cover_every_non_native_pair() -> None:
    native_pairs = frozenset({("f64", "f32")})
    exact_recipe_pairs = {
        _narrowing_pair(rule)
        for rule in WASM_CORE_SIMD128_CONTRACT_FRAGMENT.cases
        if isinstance(rule, DescriptorRule)
        and rule.source_op is conversion.scalar_fptrunc
        and rule.report_key.startswith("exact_")
    }

    assert exact_recipe_pairs == _expected_narrowing_pairs() - native_pairs


def test_bit_operations_use_native_instructions_for_each_integer_carrier() -> None:
    for scalar_op, index_op, instruction in (
        (bitwise.scalar_rotli, index.index_rotli, "rotl"),
        (bitwise.scalar_rotri, index.index_rotri, "rotr"),
        (bitwise.scalar_ctlzi, index.index_ctlzi, "clz"),
        (bitwise.scalar_cttzi, index.index_cttzi, "ctz"),
        (bitwise.scalar_ctpopi, index.index_ctpopi, "popcnt"),
    ):
        actual = {}
        for rule in WASM_CORE_SIMD128_CONTRACT_FRAGMENT.cases:
            if not isinstance(rule, DescriptorRule):
                continue
            if rule.source_op not in (scalar_op, index_op):
                continue
            result_guard = next(
                guard
                for guard in rule.guards
                if guard.kind is GuardKind.VALUE_TYPE and guard.field == "result"
            )
            assert len(rule.emit) == 1
            actual[(rule.source_op, result_guard.type_pattern.elements[0])] = rule.emit[
                0
            ].descriptor.key
        assert actual == {
            (scalar_op, "i32"): f"wasm.i32.{instruction}",
            (scalar_op, "i64"): f"wasm.i64.{instruction}",
            (index_op, "index"): f"wasm.i32.{instruction}",
        }
