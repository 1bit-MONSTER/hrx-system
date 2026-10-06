# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Direct type bindings on region argument declarations."""

import pytest

from loom.builtin_types import ALL_BUILTIN_TYPES
from loom.dialect.func import ALL_FUNC_OPS
from loom.dialect.scf import ALL_SCF_OPS
from loom.dialect.test import ALL_TEST_OPS
from loom.format.bytecode.reader import read_module
from loom.format.bytecode.writer import write_module
from loom.format.text.parser import ParseError, Parser
from loom.format.text.printer import Printer
from loom.ir import F32, DynamicDim, DynamicEncoding
from loom.type_binding import remap_value_bindings
from loom.verify import verify_module


def _formats() -> tuple[Parser, Printer]:
    parser = Parser()
    printer = Printer()
    for format in (parser, printer):
        format.register_types(ALL_BUILTIN_TYPES)
        for operations in (ALL_FUNC_OPS, ALL_SCF_OPS, ALL_TEST_OPS):
            format.register_ops(operations)
    return parser, printer


def _condition_loop_source(
    body_extent_reference: str = "body_extent",
    *,
    isolate_entry_mismatch: bool = False,
) -> str:
    yielded_state = (
        "%input, %extent, %other_extent, %layout\n"
        "        : tile<[%extent]xf32, %layout>, index, index, encoding"
        if isolate_entry_mismatch
        else (
            f"%body_view, %body_extent, %body_other_extent, %body_layout\n"
            f"        : tile<[%{body_extent_reference}]xf32, %body_layout>, "
            "index, index, encoding"
        )
    )
    return f"""\
func.def @f(%condition: i1, %extent: index, %other_extent: index,
    %layout: encoding, %input: tile<[%extent]xf32, %layout>) {{
  %result_view, %result_extent, %result_other_extent, %result_layout = scf.while(
      %before_view = %input : tile<[%before_extent]xf32, %before_layout>,
      %before_extent = %extent : index,
      %before_other_extent = %other_extent : index,
      %before_layout = %layout : encoding)
      -> (tile<[%result_extent]xf32, %result_layout>, index, index, encoding) {{
    scf.condition %condition, %before_view, %before_extent, %before_other_extent, %before_layout
        : i1, tile<[%before_extent]xf32, %before_layout>, index, index, encoding
  }} do(%body_view: tile<[%{body_extent_reference}]xf32, %body_layout>,
      %body_extent: index, %body_other_extent: index,
      %body_layout: encoding) {{
    scf.yield {yielded_state}
  }}
  func.return
}}
"""


@pytest.mark.parametrize("argument_form", ["capture", "explicit", "element"])
def test_region_argument_type_bindings(argument_form: str) -> None:
    parser, printer = _formats()
    bodies = {
        "capture": (
            "  %result = scf.for %iv = [%lower to %upper step %step]"
            "(%iter = %input : tile<[%extent]xf32, %layout>) "
            "-> (tile<[%extent]xf32, %layout>) {\n"
            "    scf.yield %iter : tile<[%extent]xf32, %layout>\n"
            "  }\n"
        ),
        "explicit": (
            "  test.block_args %input : tile<[%extent]xf32, %layout> "
            "do(%iter: tile<[%extent]xf32, %layout>) {\n"
            "    test.use %iter : tile<[%extent]xf32, %layout>\n"
            "    test.yield\n"
            "  }\n"
        ),
        "element": (
            "  %result = test.map(%iter = %input : tile<[%extent]xf32, %layout>) {\n"
            "    test.yield %iter : f32\n"
            "  } -> (tile<[%extent]xf32, %layout>)\n"
        ),
    }
    module = parser.parse(
        "func.def @f(%extent: index, %layout: encoding, "
        "%input: tile<[%extent]xf32, %layout>, "
        "%lower: index, %upper: index, %step: index) {\n"
        + bodies[argument_form]
        + "  test.use %input : tile<[%extent]xf32, %layout>\n"
        "  func.return\n}\n",
        verify=True,
    )
    text = printer.print_module(module)
    loaded_text = parser.parse(text, verify=True)
    assert printer.print_module(loaded_text) == text
    for candidate in (module, loaded_text, read_module(write_module(module))):
        entry = candidate.body.ops[0].regions[0].blocks[0]
        nested = entry.ops[0].regions[0].blocks[0]
        argument_id = nested.arg_ids[-1]
        argument = candidate.values[argument_id]
        if argument_form == "element":
            assert argument.type == F32
        else:
            assert argument.type.dims == (DynamicDim(entry.arg_ids[0]),)
            assert argument.type.encoding == DynamicEncoding(entry.arg_ids[1])
        assert nested.ops[0].operands[0] == argument_id
        assert entry.ops[1].operands[0] == entry.arg_ids[2]


def test_capture_binding_is_not_visible_after_its_region() -> None:
    parser, _ = _formats()
    with pytest.raises(ParseError, match="undefined SSA value '%iter'"):
        parser.parse(
            "func.def @f(%input: tile<4xf32>) {\n"
            "  %result = test.map(%iter = %input : tile<4xf32>) {\n"
            "    test.yield %iter : f32\n"
            "  } -> (tile<4xf32>)\n"
            "  test.use %iter : f32\n"
            "  func.return\n"
            "}\n"
        )


def test_explicit_region_args_can_reference_later_peers() -> None:
    parser, printer = _formats()
    module = parser.parse(
        "func.def @f(%extent: index, %layout: encoding, "
        "%input: tile<[%extent]xf32, %layout>) {\n"
        "  test.block_args %input, %extent, %layout "
        ": tile<[%extent]xf32, %layout>, index, encoding "
        "do(%view: tile<[%local_extent]xf32, %local_layout>, "
        "%local_extent: index, %local_layout: encoding) {\n"
        "    test.use %view : tile<[%local_extent]xf32, %local_layout>\n"
        "    test.yield\n"
        "  }\n"
        "  func.return\n"
        "}\n"
    )
    text = printer.print_module(module)
    loaded_text = parser.parse(text)
    assert printer.print_module(loaded_text) == text

    for candidate in (module, loaded_text, read_module(write_module(module))):
        function_entry = candidate.body.ops[0].regions[0].blocks[0]
        nested_entry = function_entry.ops[0].regions[0].blocks[0]
        view = candidate.values[nested_entry.arg_ids[0]]
        assert view.type.dims == (DynamicDim(nested_entry.arg_ids[1]),)
        assert view.type.encoding == DynamicEncoding(nested_entry.arg_ids[2])


def test_projected_region_argument_groups_round_trip_as_one_entry() -> None:
    parser, printer = _formats()
    module = parser.parse(
        "func.def @f() {\n"
        "  test.block_arg_groups actual(%actual: f32) "
        "expected(%expected: f32, %expected_index: index) {\n"
        "    test.use %actual : f32\n"
        "    test.use %expected : f32\n"
        "    test.use %expected_index : index\n"
        "    test.yield\n"
        "  }\n"
        "  func.return\n"
        "}\n"
    )

    text = printer.print_module(module)
    loaded = parser.parse(text)
    assert printer.print_module(loaded) == text
    operation = module.body.ops[0].regions[0].blocks[0].ops[0]
    assert operation.attributes["actual_count"] == 1
    assert len(operation.regions[0].blocks[0].arg_ids) == 3


def test_unresolved_explicit_region_peer_is_rejected() -> None:
    parser, _ = _formats()
    with pytest.raises(
        ParseError,
        match="unresolved forward reference to '%missing' in region arguments",
    ):
        parser.parse(
            "func.def @f(%input: tile<4xf32>) {\n"
            "  test.block_args %input : tile<4xf32> "
            "do(%view: tile<[%missing]xf32>) {\n"
            "    test.yield\n"
            "  }\n"
            "  func.return\n"
            "}\n"
        )


def test_condition_loop_preserves_independent_entry_schemes() -> None:
    parser, printer = _formats()
    module = parser.parse(_condition_loop_source(), verify=True)
    text = printer.print_module(module)
    loaded_text = parser.parse(text, verify=True)
    assert printer.print_module(loaded_text) == text

    for candidate in (module, loaded_text, read_module(write_module(module))):
        diagnostics = verify_module(candidate)
        assert not diagnostics.has_errors, str(diagnostics.diagnostics)
        function_entry = candidate.body.ops[0].regions[0].blocks[0]
        loop = function_entry.ops[0]
        before_entry = loop.regions[0].blocks[0]
        body_entry = loop.regions[1].blocks[0]
        result_view = candidate.values[loop.results[0]]
        before_view = candidate.values[before_entry.arg_ids[0]]
        body_view = candidate.values[body_entry.arg_ids[0]]
        assert result_view.type.dims == (DynamicDim(loop.results[1]),)
        assert result_view.type.encoding == DynamicEncoding(loop.results[3])
        assert before_view.type.dims == (DynamicDim(before_entry.arg_ids[1]),)
        assert before_view.type.encoding == DynamicEncoding(before_entry.arg_ids[3])
        assert body_view.type.dims == (DynamicDim(body_entry.arg_ids[1]),)
        assert body_view.type.encoding == DynamicEncoding(body_entry.arg_ids[3])


def test_condition_loop_rejects_wrong_entry_peer() -> None:
    parser, _ = _formats()
    module = parser.parse(
        _condition_loop_source(
            "body_other_extent",
            isolate_entry_mismatch=True,
        )
    )

    diagnostics = verify_module(module)

    assert diagnostics.has_errors
    assert any(
        diagnostic.error_id == "ERR_TYPE_013"
        and "region 'after' entry" in " ".join(diagnostic.details)
        for diagnostic in diagnostics.diagnostics
    )


@pytest.mark.parametrize(
    ("edge", "error_id", "constraint_name"),
    [
        ("initial", "ERR_TYPE_001", None),
        ("yield", "ERR_TYPE_009", "YieldTypesMatch"),
        (
            "condition",
            "ERR_TYPE_001",
            "ConditionForwardedTypesMatchBlockArgs",
        ),
    ],
)
def test_condition_loop_rejects_wrong_edge_peer(
    edge: str,
    error_id: str,
    constraint_name: str | None,
) -> None:
    parser, _ = _formats()
    module = parser.parse(_condition_loop_source())
    function_entry = module.body.ops[0].regions[0].blocks[0]
    loop = function_entry.ops[0]
    before_entry = loop.regions[0].blocks[0]
    body_entry = loop.regions[1].blocks[0]

    if edge == "initial":
        initial = module.values[loop.operands[0]]
        initial.type = remap_value_bindings(
            (initial.type,),
            {loop.operands[1]: loop.operands[2]},
        )[0]
    elif edge == "yield":
        body_entry.ops[-1].operands[0] = loop.operands[0]
    else:
        before_entry.ops[-1].operands[1] = loop.operands[0]

    diagnostics = verify_module(module)

    assert diagnostics.has_errors
    assert any(
        diagnostic.error_id == error_id
        and (
            constraint_name is None
            or diagnostic.message == f"{constraint_name} constraint violated"
        )
        for diagnostic in diagnostics.diagnostics
    )
