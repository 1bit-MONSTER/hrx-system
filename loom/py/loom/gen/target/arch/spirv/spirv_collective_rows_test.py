# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

from loom.gen.target.arch.spirv.spirv_collective_rows import generate_tables
from loom.gen.target.arch.spirv.spirv_workgroup_rows import (
    _combine,
    arithmetic_rows,
    generate_workgroup_tables,
    source_arithmetic_indices,
)
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


def test_workgroup_family_covers_native_types_and_kinds() -> None:
    indices = source_arithmetic_indices()
    integer_kinds = ("addi", "muli", "minsi", "maxsi", "minui", "maxui", "andi", "ori", "xori")
    float_kinds = ("addf", "mulf", "minnumf", "maxnumf", "minimumf", "maximumf")
    assert set(indices) == {
        (source, kind)
        for sources, kinds in (
            (("i1", "i8", "i16", "i32", "i64"), integer_kinds),
            (("f16", "f32", "f64"), float_kinds),
        )
        for source in sources
        for kind in kinds
    }
    rows = arithmetic_rows()
    assert len(rows) == 45
    assert set(indices.values()) == set(range(len(rows)))
    for width in (16, 32, 64):
        for kind, integer_kind in (("minnumf", "minsi"), ("minimumf", "minsi"), ("maxnumf", "maxsi"), ("maximumf", "maxsi")):
            assert indices[f"f{width}", kind] == indices[f"i{width}", integer_kind]


def test_workgroup_integer_identities_and_combines_match_source_semantics() -> None:
    indices = source_arithmetic_indices()
    rows = arithmetic_rows()
    for width in (8, 16, 32, 64):
        expected = {
            "addi": ("iadd", None, 0),
            "muli": ("imul", None, 1),
            "minsi": ("s_less_than", "select", (1 << (width - 1)) - 1),
            "maxsi": ("s_greater_than", "select", -(1 << (width - 1))),
            "minui": ("u_less_than", "select", -1),
            "maxui": ("u_greater_than", "select", 0),
            "andi": ("bitwise_and", None, -1),
            "ori": ("bitwise_or", None, 0),
            "xori": ("bitwise_xor", None, 0),
        }
        for kind, combine in expected.items():
            assert _combine(rows[indices[f"i{width}", kind]]) == combine
    for kinds, combine in (
        (("addi", "xori"), ("logical_not_equal", None, 0)),
        (("muli", "maxsi", "minui", "andi"), ("logical_and", None, 1)),
        (("minsi", "maxui", "ori"), ("logical_or", None, 0)),
    ):
        for kind in kinds:
            assert _combine(rows[indices["i1", kind]]) == combine


def test_workgroup_rows_share_transport_and_preserve_native_float_constants() -> None:
    tables = generate_workgroup_tables()
    assert tables.count(".byte_count = ") == 12
    assert tables.count(".identity_bits = ") == 45
    assert tables.count(".arithmetic = ") == 8
    assert "typedef" not in tables
    assert "iree_status_t" not in tables
    rows = arithmetic_rows()
    indices = source_arithmetic_indices()
    for width, one in ((16, 0x3C00), (32, 0x3F800000), (64, 0x3FF0000000000000)):
        assert _combine(rows[indices[f"f{width}", "addf"]]) == ("fadd", None, 0)
        assert _combine(rows[indices[f"f{width}", "mulf"]]) == ("fmul", None, one)
        assert f".constant = SPIRV_LOGICAL_CORE_DESCRIPTOR_REF_OP_CONSTANT_F{width}" in tables
        assert f".input_cast = SPIRV_LOGICAL_CORE_DESCRIPTOR_REF_OP_BITCAST_I{width}_F{width}" not in tables
