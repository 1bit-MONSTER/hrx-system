# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

from loom.dialect.scalar import ALL_SCALAR_OPS
from loom.dialect.scalar.arithmetic import ALL_ARITHMETIC_OPS
from loom.dialect.scalar.comparison import ALL_COMPARISON_OPS
from loom.dialect.scalar.math import ALL_MATH_OPS
from loom.dsl import FACT_IDENTITY, FLOAT, POISON, SAFE_TO_SPECULATE, OpPhase

_UNIT_EAGER_FLOAT_SUFFIXES = {
    "absf",
    "addf",
    "ceilf",
    "clampf",
    "cmpf",
    "copysignf",
    "divf",
    "floorf",
    "fmaf",
    "isfinitef",
    "isinff",
    "isnanf",
    "maximumf",
    "maxnumf",
    "minimumf",
    "minnumf",
    "mulf",
    "negf",
    "roundevenf",
    "roundf",
    "signf",
    "subf",
    "truncf",
}


def test_scalar_operation_phases_are_complete() -> None:
    operation_by_name = {operation.name: operation for operation in ALL_SCALAR_OPS}
    non_executable_names = {operation.name for operation in ALL_SCALAR_OPS if operation.effective_phase is not OpPhase.EXECUTABLE}

    assert non_executable_names == {
        "scalar.assume",
        "scalar.constexpr",
        "scalar.poison",
    }
    assert operation_by_name["scalar.assume"].has_trait(FACT_IDENTITY.name)
    assert operation_by_name["scalar.constexpr"].effective_phase is OpPhase.COMPILE_TIME_QUERY
    assert not operation_by_name["scalar.constexpr"].has_trait(FACT_IDENTITY.name)
    assert operation_by_name["scalar.poison"].has_trait(POISON.name)


def test_total_scalar_float_families_author_speculation_policy() -> None:
    arithmetic_ops = tuple(op for op in ALL_ARITHMETIC_OPS if any(field.type_constraint is FLOAT for field in (*op.operands, *op.results)))
    comparison_ops = tuple(op for op in ALL_COMPARISON_OPS if any(field.type_constraint is FLOAT for field in (*op.operands, *op.results)))
    total_float_ops = (*arithmetic_ops, *ALL_MATH_OPS, *comparison_ops)

    for op in total_float_ops:
        suffix = op.name.split(".", 1)[1]
        assert SAFE_TO_SPECULATE in op.traits, op.name
        assert op.eager_execution_cost == (1 if suffix in _UNIT_EAGER_FLOAT_SUFFIXES else 0), op.name
