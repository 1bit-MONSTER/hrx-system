# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Declaration conflicts in the generated public operation API."""

from dataclasses import replace

import pytest

from loom.assembly import AttrDict, ScopedEnumRef
from loom.dsl import ATTR_TYPE_I64, AttrDef, Dialect, EnumCase, EnumDef, Op, TargetLikeInterface
from loom.gen.ops.c_ops_header import generate_ops_h, generate_ops_inc


def test_optional_attribute_presence_rejects_accessor_name_collisions() -> None:
    fields = [AttrDef("count", ATTR_TYPE_I64, optional=True), AttrDef("has_count", ATTR_TYPE_I64)]
    for attrs in (fields, list(reversed(fields))):
        op = Op("test.presence", group=Dialect("test"), attrs=attrs, format=[AttrDict()])
        with pytest.raises(ValueError, match="presence accessor 'loom_test_presence_has_count' conflicts with field 'has_count'"):
            generate_ops_h("test", 0, [op])
        with pytest.raises(ValueError, match="presence accessor 'loom_test_presence_has_count' conflicts with field 'has_count'"):
            generate_ops_inc([op])


def test_attribute_rewriting_rejects_accessor_name_collisions() -> None:
    fields = [AttrDef("count", ATTR_TYPE_I64), AttrDef("rewrite_count", ATTR_TYPE_I64)]
    for attrs in (fields, list(reversed(fields))):
        op = Op("test.mutation", group=Dialect("test"), attrs=attrs, format=[AttrDict()])
        with pytest.raises(ValueError, match="rewrite accessor 'loom_test_mutation_rewrite_count' conflicts with field 'rewrite_count'"):
            generate_ops_h("test", 0, [op])


def test_attribute_helpers_reject_accessor_name_collisions() -> None:
    for name, kind in (("set_count", "setter"), ("count_attr", "attribute"), ("count_descriptor", "descriptor"), ("initialize_count", "initializer"), ("count_diagnostic_ref", "diagnostic")):
        fields = [AttrDef("count", ATTR_TYPE_I64), AttrDef(name, ATTR_TYPE_I64)]
        for attrs in (fields, list(reversed(fields))):
            op = Op("test.mutation", group=Dialect("test"), attrs=attrs, format=[AttrDict()])
            expected = f"{kind} accessor 'loom_test_mutation_{name}' conflicts with field '{name}'"
            with pytest.raises(ValueError, match=expected):
                generate_ops_h("test", 0, [op])
            with pytest.raises(ValueError, match=expected):
                generate_ops_inc([op])


def test_dictionary_updates_require_dictionary_fields() -> None:
    for field_type in ("dict", ATTR_TYPE_I64):
        op = Op("test.update", group=Dialect("test"), attrs=[AttrDef("payload", field_type)])
        op = replace(op, attrs=[*op.attrs, AttrDef("update_payload", ATTR_TYPE_I64)])
        if field_type == "dict":
            with pytest.raises(ValueError, match="dictionary update accessor"):
                generate_ops_inc([op])
        else:
            generate_ops_inc([op])


def test_target_record_readers_reject_accessor_name_collisions() -> None:
    op = Op(
        "test.target",
        group=Dialect("test"),
        attrs=[
            AttrDef("symbol", "symbol"),
            AttrDef("kind", "enum", enum_def=EnumDef("Kind", [EnumCase("generic", 0)])),
            AttrDef("kind_from_record", ATTR_TYPE_I64),
        ],
        interfaces=[TargetLikeInterface(symbol="symbol", selector="kind")],
    )
    with pytest.raises(ValueError, match="record accessor"):
        generate_ops_inc([op])
    generate_ops_inc([replace(op, interfaces=[])])


def test_scoped_enum_rejects_context_free_c_builder() -> None:
    op = Op(
        "test.packet",
        group=Dialect("test"),
        attrs=[AttrDef("descriptor", "scoped_enum")],
        format=[ScopedEnumRef("descriptor")],
    )

    with pytest.raises(ValueError, match="requires a domain-aware handwritten C builder"):
        generate_ops_h("test", 0, [op])


def test_external_flags_use_the_owning_header() -> None:
    flags = EnumDef(
        "Flags",
        [EnumCase("preserve", 2)],
        c_type="test_flags_t",
        c_const_prefix="TEST_FLAG",
        c_include="test/flags.h",
    )
    op = Op(
        "test.update",
        group=Dialect("test"),
        attrs=[AttrDef("flags", "flags", optional=True, enum_def=flags)],
    )
    header = generate_ops_h("test", 0, [op])
    assert '#include "test/flags.h"' in header
    assert "#define LOOM_TEST_FLAGS_PRESERVE" not in header
    assert "LOOM_DEFINE_INSTANCE_FLAGS(loom_test_update_flags)" in header
