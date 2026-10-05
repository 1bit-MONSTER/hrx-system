# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Trait derivation contracts for generated operation metadata."""

import pytest

from loom.dsl import (
    CONTEXTUAL,
    CONVERGENT,
    DECOMPOSABLE,
    ELEMENTWISE,
    NON_DETERMINISTIC,
    OBSERVABLE_EFFECT,
    POISON_BOUNDARY,
    SCALAR,
    TERMINATOR,
    VECTOR,
    Dialect,
    Op,
    Operand,
    Result,
    SameShape,
    SameType,
    Trait,
)
from loom.gen.ops.c_metadata_tables import generate_tables_c
from loom.gen.ops.c_traits import (
    _is_explicit_vector_decomposable,
    _is_shape_preserving_elementwise_vector_decomposable,
    trait_flags,
)


def test_decomposition_requires_shape_preservation_through_results() -> None:
    for relation in (SameType, SameShape):
        for covers_result in (False, True):
            fields = ("lhs", "rhs", "result") if covers_result else ("lhs", "rhs")
            op = Op(
                "test.elementwise",
                group=Dialect("test"),
                operands=[Operand("lhs", VECTOR), Operand("rhs", VECTOR)],
                results=[Result("result", VECTOR)],
                constraints=[relation(*fields)],
                traits=[ELEMENTWISE],
            )
            assert _is_shape_preserving_elementwise_vector_decomposable(op) == covers_result


def test_explicit_decomposition_accepts_shape_preserving_ops() -> None:
    op = Op(
        "test.elementwise",
        group=Dialect("test"),
        operands=[Operand("input", VECTOR)],
        results=[Result("result", VECTOR)],
        constraints=[SameType("input", "result")],
        traits=[ELEMENTWISE, DECOMPOSABLE],
    )
    generate_tables_c("test", 0, [op])


def test_explicit_decomposition_accepts_invariant_scalar_captures() -> None:
    op = Op(
        "test.splat",
        group=Dialect("test"),
        operands=[Operand("scalar", SCALAR)],
        results=[Result("result", VECTOR)],
        traits=[DECOMPOSABLE],
    )

    assert _is_explicit_vector_decomposable(op)
    generate_tables_c("test", 0, [op])


@pytest.mark.parametrize(
    "semantic_trait",
    [
        NON_DETERMINISTIC,
        OBSERVABLE_EFFECT,
        CONVERGENT,
        CONTEXTUAL,
        POISON_BOUNDARY,
        TERMINATOR,
    ],
)
def test_decomposition_requires_rematerializable_semantics(
    semantic_trait: Trait,
) -> None:
    op = Op(
        "test.elementwise",
        group=Dialect("test"),
        operands=[Operand("input", VECTOR)],
        results=[Result("result", VECTOR)],
        constraints=[SameType("input", "result")],
        traits=[ELEMENTWISE, DECOMPOSABLE, semantic_trait],
    )

    assert not _is_shape_preserving_elementwise_vector_decomposable(op)
    with pytest.raises(ValueError, match="Decomposable requires"):
        generate_tables_c("test", 0, [op])


def test_generated_purity_matches_dsl_semantics() -> None:
    op = Op("test.observe", traits=[OBSERVABLE_EFFECT])

    assert not op.is_pure
    assert "LOOM_TRAIT_PURE" not in trait_flags(op)


def test_generate_tables_rejects_explicit_decomposable_for_mixed_result_elementwise_ops() -> None:
    op = Op(
        "test.cmp",
        group=Dialect("test"),
        operands=[Operand("lhs", VECTOR), Operand("rhs", VECTOR)],
        results=[Result("result", VECTOR)],
        constraints=[SameType("lhs", "rhs")],
        traits=[ELEMENTWISE, DECOMPOSABLE],
    )

    with pytest.raises(ValueError, match="Decomposable requires"):
        generate_tables_c("test", 0x01, [op])
