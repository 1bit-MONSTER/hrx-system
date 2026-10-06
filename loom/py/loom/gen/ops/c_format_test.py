# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

from itertools import permutations

import pytest

from loom.assembly import LPAREN, RPAREN, AssemblyFormat, Attr, BindingList, BlockArgs, Clause, OptionalGroup, Ref, Refs, Region
from loom.dsl import ATTR_TYPE_I64, INTEGER, AttrDef, Dialect, Op, Operand, RegionDef
from loom.gen.ops.c_format import region_entry_args_declared_by_parent, translate_format_elements
from loom.gen.ops.c_metadata_tables import generate_tables_c


def _assert_invalid_format(op: Op, expected: str) -> None:
    with pytest.raises(ValueError, match=expected):
        generate_tables_c("test", 0, [op])


def test_optional_clauses_follow_declaration_order() -> None:
    operands = [
        Operand("values", INTEGER, variadic=True),
        Operand("depth", INTEGER, optional=True),
        Operand("factor", INTEGER, optional=True),
    ]
    depth = OptionalGroup([Clause("pipeline", Ref("depth"))], anchor="depth")
    factor = OptionalGroup([Clause("unroll", Ref("factor"))], anchor="factor")
    for clauses in ([depth, factor], [factor, depth]):
        op = Op(
            "test.policies",
            group=Dialect("test"),
            operands=operands,
            format=[Refs("values"), *clauses],
        )
        if clauses[0] is depth:
            generate_tables_c("test", 0, [op])
        else:
            _assert_invalid_format(op, "segmented operands must appear in declaration order; 'depth' follows 'factor'")


def test_variadic_groups_follow_declaration_order() -> None:
    op = Op(
        "test.groups",
        group=Dialect("test"),
        operands=[
            Operand("first", INTEGER, variadic=True),
            Operand("second", INTEGER, variadic=True),
        ],
        format=[Refs("second"), Refs("first")],
    )
    _assert_invalid_format(op, "segmented operands must appear in declaration order; 'first' follows 'second'")


def test_fixed_operands_can_use_format_order() -> None:
    op = Op(
        "test.fixed",
        group=Dialect("test"),
        operands=[Operand("first", INTEGER), Operand("second", INTEGER)],
        format=[Ref("second"), Ref("first")],
    )
    generate_tables_c("test", 0, [op])


def test_entry_argument_ownership_follows_region_clauses() -> None:
    for names in permutations(("body", "plain", "after")):
        op = Op(
            "test.regions",
            group=Dialect("test"),
            operands=[Operand("captures", INTEGER, variadic=True)],
            regions=[RegionDef(name) for name in names],
            format=[BindingList("captures"), Region("body"), Region("plain"), BlockArgs("after"), Region("after")],
        )
        declared = region_entry_args_declared_by_parent(op, translate_format_elements(op))
        assert declared == {names.index("body"), names.index("after")}


def test_projected_block_arguments_encode_signature_boundaries() -> None:
    op = Op(
        "test.partitioned_region",
        group=Dialect("test"),
        attrs=[AttrDef("actual_count", ATTR_TYPE_I64)],
        regions=[RegionDef("body")],
        format=[
            BlockArgs(
                "body",
                group="actual",
                end_attr="actual_count",
            ),
            BlockArgs(
                "body",
                group="expected",
                start_attr="actual_count",
            ),
            Region("body"),
        ],
    )

    assert translate_format_elements(op) == [
        (
            "LOOM_FORMAT_KIND_BLOCK_ARGS",
            0,
            "LOOM_FORMAT_BLOCK_ARGS_DATA(255, 0)",
        ),
        (
            "LOOM_FORMAT_KIND_BLOCK_ARGS",
            0,
            "LOOM_FORMAT_BLOCK_ARGS_DATA(0, 255)",
        ),
        ("LOOM_FORMAT_KIND_REGION", 0, "LOOM_REGION_SYNTAX_DEFAULT"),
    ]


def test_inverted_optional_group_encodes_anchor_polarity() -> None:
    op = Op(
        "test.optional_alternative",
        group=Dialect("test"),
        attrs=[AttrDef("count", ATTR_TYPE_I64, optional=True)],
        format=[
            OptionalGroup([Attr("count")], anchor="count"),
            OptionalGroup([LPAREN, RPAREN], anchor="count", inverted=True),
        ],
    )

    assert translate_format_elements(op) == [
        ("LOOM_FORMAT_KIND_OPTIONAL_GROUP", 0, "(1 << 2) | 1"),
        ("LOOM_FORMAT_KIND_ATTR_VALUE", 0, "0"),
        (
            "LOOM_FORMAT_KIND_OPTIONAL_GROUP",
            0,
            "LOOM_FORMAT_OPTIONAL_GROUP_DATA(2, 1, true)",
        ),
        ("LOOM_FORMAT_KIND_KEYWORD", 0, "LOOM_KW_LPAREN"),
        ("LOOM_FORMAT_KIND_KEYWORD", 0, "LOOM_KW_RPAREN"),
    ]


def test_induction_variable_declaration_belongs_to_next_region() -> None:
    op = Op(
        "test.loop",
        group=Dialect("test"),
        regions=[RegionDef("body"), RegionDef("after")],
        format=[Ref("iv"), Region("body"), Region("after")],
    )
    assert region_entry_args_declared_by_parent(op, translate_format_elements(op)) == {0}


def test_assembly_format_binds_reordered_fields_to_canonical_layout() -> None:
    for names in (("first", "second"), ("second", "first")):
        op = Op(
            "test.fields",
            group=Dialect("test"),
            operands=[Operand("value", INTEGER)],
            attrs=[AttrDef(name, ATTR_TYPE_I64) for name in names],
            format=[Ref("value"), Attr("first"), Attr("second")],
            assembly=AssemblyFormat("fields", [Attr("second"), Ref("value"), Attr("first")]),
        )
        elements = translate_format_elements(op, op.assembly.elements)
        assert [index for _, index, _ in elements] == [names.index("second"), 0, names.index("first")]


def test_unknown_region_syntax_is_rejected() -> None:
    op = Op(
        "test.region_syntax",
        group=Dialect("test"),
        regions=[RegionDef("body")],
        format=[Region("body", syntax="missing.syntax")],
    )

    with pytest.raises(ValueError, match=r"Op 'test\.region_syntax': unknown region syntax 'missing\.syntax'"):
        generate_tables_c("test", 0, [op])
