# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

from loom.gen.target.arch.spirv.spirv_collective_rows import generate_tables
from loom.target.arch.spirv.subgroup import (
    SPIRV_COLLECTIVE_EXTREMA_TYPES,
    SPIRV_SUBGROUP_ARITHMETIC,
)


def test_extrema_rows_cover_each_native_carrier_once() -> None:
    assert [(row.bit_count, row.lane_count) for row in SPIRV_COLLECTIVE_EXTREMA_TYPES] == [(width, lanes) for width in (16, 32, 64) for lanes in (1, 2, 3, 4)]
    native = {row.instruction.key: row for row in SPIRV_SUBGROUP_ARITHMETIC}
    for row in SPIRV_COLLECTIVE_EXTREMA_TYPES:
        assert ("construct" in row.descriptors) == (row.lane_count != 1)
        for mode in ("reduce", "clustered_reduce", "inclusive_scan", "exclusive_scan"):
            for minimum in (True, False):
                arithmetic = native[row.subgroup_key(mode, minimum=minimum)]
                assert arithmetic.source_type == f"i{row.bit_count}"
                assert arithmetic.combining_kinds == (("minsi",) if minimum else ("maxsi",))
                assert arithmetic.instruction.group_operation == f"LOOM_SPIRV_GROUP_OPERATION_{mode.upper()}"


def test_generation_emits_only_compact_descriptor_rows() -> None:
    tables = generate_tables()
    assert tables.count(".bit_count = ") == 12
    assert tables.count(".construct = ") == 9
    assert tables.count(".subgroup = ") == 12
    assert tables.count("DESCRIPTOR_REF_OP_GROUP_NON_UNIFORM_SMIN_") == 48
    assert tables.count("DESCRIPTOR_REF_OP_GROUP_NON_UNIFORM_SMAX_") == 48
    assert "typedef" not in tables
    assert "iree_status_t" not in tables
