# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Tests for AMDGPU comparison contract source tables."""

from __future__ import annotations

from loom.dialect.scalar import comparison as scalar
from loom.dialect.vector import defs as vector
from loom.dsl import Op
from loom.target.arch.amdgpu.contracts.compare import (
    AMDGPU_COMPARE_CONTRACT_DIALECT_OPS,
    AMDGPU_COMPARE_CONTRACT_FRAGMENT,
)
from loom.target.contracts import (
    CompiledLowerRuleSet,
    LowerRule,
    compile_lower_rule_set,
)


def _compiled_rules() -> CompiledLowerRuleSet:
    return compile_lower_rule_set(
        AMDGPU_COMPARE_CONTRACT_FRAGMENT,
        dialect_ops=AMDGPU_COMPARE_CONTRACT_DIALECT_OPS,
    )


def _rules_for_source_op(
    compiled: CompiledLowerRuleSet, source_op: Op
) -> tuple[LowerRule, ...]:
    for span in compiled.spans:
        if span.source_op is source_op:
            return compiled.rules[span.rule_start : span.rule_start + span.rule_count]
    raise AssertionError(f"no lower-rule span for {source_op.name}")


def _descriptor_sequences(
    compiled: CompiledLowerRuleSet, source_op: Op
) -> tuple[tuple[str, ...], ...]:
    return tuple(
        tuple(
            compiled.emits[emit_index].descriptor.key
            for emit_index in range(rule.emit_start, rule.emit_start + rule.emit_count)
        )
        for rule in _rules_for_source_op(compiled, source_op)
    )


def test_scalar_float_classification_selects_cheapest_constant_form() -> None:
    compiled = _compiled_rules()
    assert _descriptor_sequences(compiled, scalar.scalar_isnanf) == tuple(
        (f"amdgpu.v_cmp_class_f{bit_width}.classes_inline",)
        for bit_width in (16, 32, 64)
    )
    for source_op in (scalar.scalar_isinff, scalar.scalar_isfinitef):
        assert _descriptor_sequences(compiled, source_op) == tuple(
            sequence
            for bit_width in (16, 32, 64)
            for sequence in (
                (f"amdgpu.v_cmp_class_f{bit_width}.classes_lit",),
                ("amdgpu.s_mov_b32", f"amdgpu.v_cmp_class_f{bit_width}"),
            )
        )


def test_vector_float_classification_publishes_all_native_widths() -> None:
    compiled = _compiled_rules()
    for source_op in (
        vector.vector_isnanf,
        vector.vector_isinff,
        vector.vector_isfinitef,
    ):
        rules = _rules_for_source_op(compiled, source_op)
        assert len(rules) == 3
        assert all(rule.emit_count == 0 for rule in rules)
