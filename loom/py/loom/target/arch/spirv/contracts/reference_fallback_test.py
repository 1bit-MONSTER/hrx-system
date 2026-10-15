# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

from loom.dialect.scf import defs as scf
from loom.dialect.vector import defs as vector
from loom.target.arch.spirv.contracts.logical_core import (
    SPIRV_LOGICAL_CORE_CONTRACT_FRAGMENT,
)
from loom.target.arch.spirv.contracts.reference_fallback import (
    SPIRV_REFERENCE_FALLBACK_CONTRACT_CASES,
    SPIRV_REFERENCE_FALLBACK_OPS,
)
from loom.target.contracts import UnsupportedRule


def test_reference_fallback_family_is_complete_and_unique() -> None:
    combining_ops = (
        vector.vector_addi,
        vector.vector_addf,
        vector.vector_muli,
        vector.vector_mulf,
        vector.vector_minsi,
        vector.vector_maxsi,
        vector.vector_minui,
        vector.vector_maxui,
        vector.vector_andi,
        vector.vector_ori,
        vector.vector_xori,
        vector.vector_minimumf,
        vector.vector_maximumf,
        vector.vector_minnumf,
        vector.vector_maxnumf,
    )
    structural_ops = (
        scf.scf_select,
        vector.vector_transpose,
        vector.vector_shuffle,
        vector.vector_cmpi,
        vector.vector_cmpf,
    )
    assert SPIRV_REFERENCE_FALLBACK_OPS == (
        *combining_ops,
        *vector.VECTOR_CAST_OPS,
        *structural_ops,
    )
    assert len(SPIRV_REFERENCE_FALLBACK_OPS) == 31
    assert len(set(SPIRV_REFERENCE_FALLBACK_OPS)) == len(SPIRV_REFERENCE_FALLBACK_OPS)


def test_reference_fallback_cases_are_final_catch_all_rules() -> None:
    assert (
        tuple(rule.source_op for rule in SPIRV_REFERENCE_FALLBACK_CONTRACT_CASES)
        == SPIRV_REFERENCE_FALLBACK_OPS
    )
    assert all(
        isinstance(rule, UnsupportedRule)
        for rule in SPIRV_REFERENCE_FALLBACK_CONTRACT_CASES
    )

    fragment_cases = SPIRV_LOGICAL_CORE_CONTRACT_FRAGMENT.cases
    for fallback_rule in SPIRV_REFERENCE_FALLBACK_CONTRACT_CASES:
        matching_indices = tuple(
            index
            for index, case in enumerate(fragment_cases)
            if case.source_op is fallback_rule.source_op
        )
        assert matching_indices
        assert fragment_cases[matching_indices[-1]] is fallback_rule
        assert all(
            not isinstance(fragment_cases[index], UnsupportedRule)
            for index in matching_indices[:-1]
        )
