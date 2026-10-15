# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Explicit SPIR-V ownership of shared vector reference fallbacks."""

from loom.dialect.scf import defs as scf
from loom.dialect.vector import defs as vector
from loom.target.contracts import ContractCase, UnsupportedRule

# These operations have target-independent vector reference recipes that run
# only after a target contract explicitly rejects the authored form. Native
# SPIR-V rules precede these catch-all cases and retain selection priority.
SPIRV_REFERENCE_FALLBACK_OPS = (
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
    *vector.VECTOR_CAST_OPS,
    scf.scf_select,
    vector.vector_transpose,
    vector.vector_shuffle,
    vector.vector_cmpi,
    vector.vector_cmpf,
)


SPIRV_REFERENCE_FALLBACK_CONTRACT_CASES: tuple[ContractCase, ...] = tuple(
    UnsupportedRule(source_op=source_op) for source_op in SPIRV_REFERENCE_FALLBACK_OPS
)
