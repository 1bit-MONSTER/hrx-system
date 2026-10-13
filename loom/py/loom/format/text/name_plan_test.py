# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Definition identity through lexical text serialization."""

import pytest

from loom.builtin_types import ALL_BUILTIN_TYPES
from loom.dialect.func import ALL_FUNC_OPS
from loom.dialect.globals import ALL_GLOBAL_OPS
from loom.dialect.scf import ALL_SCF_OPS
from loom.dialect.test import ALL_TEST_OPS
from loom.format.text.parser import ParseError, Parser
from loom.format.text.printer import Printer
from loom.ir import DynamicDim, Module


def _formats() -> tuple[Parser, Printer]:
    parser = Parser()
    printer = Printer()
    parser.register_types(ALL_BUILTIN_TYPES)
    printer.register_types(ALL_BUILTIN_TYPES)
    for operations in (ALL_FUNC_OPS, ALL_GLOBAL_OPS, ALL_SCF_OPS, ALL_TEST_OPS):
        parser.register_ops(operations)
        printer.register_ops(operations)
    return parser, printer


def _roundtrip(module: Module) -> tuple[Module, str]:
    parser, printer = _formats()
    names = [value.name for value in module.values]
    text = printer.print_module(module)
    assert [value.name for value in module.values] == names
    assert printer.print_module(module) == text
    loaded = parser.parse(text)
    assert printer.print_module(loaded) == text
    return loaded, text


def test_harmless_shadowing_and_independent_functions() -> None:
    parser, _ = _formats()
    text = (
        "func.def @first(%condition: i1, %extent: index) {\n"
        "  scf.if %condition {\n"
        "    %extent = test.constant 4 : index\n"
        "    test.use %extent : index\n"
        "  }\n"
        "  test.use %extent : index\n"
        "  func.return\n"
        "}\n\n"
        "func.decl @second(%extent: index)\n"
    )
    _, printed = _roundtrip(parser.parse(text))
    assert printed == text


def test_global_symbolic_bindings_remain_local_to_each_declaration() -> None:
    parser, _ = _formats()
    text = (
        "global.constant @first : tile<[%extent]xf32> where [multiple_of(%extent, 16)]\n\n"
        "global.constant @second : tile<[%extent]xf32> where [multiple_of(%extent, 32)]\n"
    )
    loaded, printed = _roundtrip(parser.parse(text))
    assert printed == text
    first, second = loaded.body.ops
    first_extent = loaded.values[first.results[0]].type.dims[0].value_id
    second_extent = loaded.values[second.results[0]].type.dims[0].value_id
    assert first_extent != second_extent
    assert first.attributes["predicates"][0].args[0].value == first_extent
    assert second.attributes["predicates"][0].args[0].value == second_extent


@pytest.mark.parametrize(
    "reference", ["operand", "predicate", "result_type", "operand_type"]
)
def test_captured_outer_value(reference: str) -> None:
    parser, _ = _formats()
    uses = {
        "operand": "test.use %extent : index",
        "predicate": "%bounded = test.assume %condition [eq(%extent, 7)] : i1",
        "result_type": "%shaped = test.convert %condition : i1 -> vector<[%extent]xf32>",
        "operand_type": "test.use %values : vector<[%extent]xf32>",
    }
    module = parser.parse(
        "func.def @capture(%condition: i1, %extent: index, "
        "%values: vector<[%extent]xf32>) {\n"
        "  scf.if %condition {\n"
        "    %local = test.constant 4 : index\n"
        f"    {uses[reference]}\n"
        "    test.use %local : index\n"
        "  }\n"
        "  func.return\n"
        "}\n"
    )
    nested = module.body.ops[0].regions[0].blocks[0].ops[0].regions[0].blocks[0]
    module.values[nested.ops[0].results[0]].name = "extent"
    loaded, _ = _roundtrip(module)
    body = loaded.body.ops[0].regions[0].blocks[0]
    extent = body.arg_ids[1]
    nested = body.ops[0].regions[0].blocks[0]
    use = nested.ops[1]
    if reference == "operand":
        assert use.operands[0] == extent
    elif reference == "predicate":
        assert use.attributes["predicates"][0].args[0].value == extent
    elif reference == "result_type":
        assert loaded.values[use.results[0]].type.dims == (DynamicDim(extent),)
    else:
        assert loaded.values[use.operands[0]].type.dims == (DynamicDim(extent),)
    assert nested.ops[2].operands[0] == nested.ops[0].results[0]
    assert loaded.values[extent].name != loaded.values[nested.ops[0].results[0]].name


def test_captured_dynamic_encoding() -> None:
    parser, _ = _formats()
    module = parser.parse(
        "func.def @capture(%condition: i1, %layout: encoding) {\n"
        "  scf.if %condition {\n"
        "    %local = test.convert %condition : i1 -> encoding\n"
        "    %shaped = test.convert %condition : i1 -> tile<4xf32, %layout>\n"
        "    test.use %local : encoding\n"
        "  }\n"
        "  func.return\n"
        "}\n"
    )
    nested = module.body.ops[0].regions[0].blocks[0].ops[0].regions[0].blocks[0]
    module.values[nested.ops[0].results[0]].name = "layout"
    loaded, _ = _roundtrip(module)
    body = loaded.body.ops[0].regions[0].blocks[0]
    nested = body.ops[0].regions[0].blocks[0]
    assert (
        loaded.values[nested.ops[1].results[0]].type.encoding.value_id
        == body.arg_ids[1]
    )
    assert nested.ops[2].operands[0] == nested.ops[0].results[0]


def test_signature_names_and_result_bindings() -> None:
    parser, _ = _formats()
    module = parser.parse(
        "func.decl @f(%argument: index) -> (%extent: index, vector<[%extent]xf32>) "
        "where [eq(%argument, %extent)]\n"
    )
    operation = module.body.ops[0]
    module.values[operation.results[0]].name = "argument"
    loaded, _ = _roundtrip(module)
    operation = loaded.body.ops[0]
    argument = operation.operands[0]
    extent = operation.results[0]
    assert loaded.values[argument].name != loaded.values[extent].name
    assert loaded.values[operation.results[1]].type.dims == (DynamicDim(extent),)
    predicate = operation.attributes["predicates"][0]
    assert [arg.value for arg in predicate.args] == [argument, extent]


def test_tied_result_uses_the_argument_name_plan() -> None:
    parser, _ = _formats()
    module = parser.parse(
        "func.def @f(%condition: i1, %tensor: tensor<4xf32>) "
        "-> (%tensor as tensor<4xf32>) {\n"
        "  scf.if %condition {\n"
        "    %local = test.convert %condition : i1 -> tensor<4xf32>\n"
        "    test.use %tensor : tensor<4xf32>\n"
        "  }\n"
        "  func.return %tensor : tensor<4xf32>\n"
        "}\n"
    )
    nested = module.body.ops[0].regions[0].blocks[0].ops[0].regions[0].blocks[0]
    module.values[nested.ops[0].results[0]].name = "tensor"
    loaded, _ = _roundtrip(module)
    operation = loaded.body.ops[0]
    assert operation.tied_results[0].operand_index == 1
    body = operation.regions[0].blocks[0]
    assert body.ops[0].regions[0].blocks[0].ops[1].operands[0] == body.arg_ids[1]
    assert body.ops[1].operands[0] == body.arg_ids[1]


@pytest.mark.parametrize("result_name", ["extent", "", "input"])
def test_tied_result_keeps_its_own_referenced_identity(result_name: str) -> None:
    parser, printer = _formats()
    module = parser.parse(
        "func.decl @shape(%input: index) -> "
        "(%extent: %input as index, tensor<[%extent]xf32>) "
        "where [range(%extent, 1, 32)]\n",
        verify=True,
    )
    module.values[module.body.ops[0].results[0]].name = result_name
    text = printer.print_module(module)
    loaded = parser.parse(text, verify=True)
    assert printer.print_module(loaded) == text
    op = loaded.body.ops[0]
    extent = op.results[0]
    assert extent != op.operands[0]
    assert op.tied_results[0].operand_index == 0
    assert op.tied_results[0].result_index == 0
    assert loaded.values[op.results[1]].type.dims == (DynamicDim(extent),)
    assert op.attributes["predicates"][0].args[0].value == extent


@pytest.mark.parametrize(
    "text",
    [
        "func.decl @missing() -> (%result: %missing as index)",
        "func.decl @not_argument() -> (%first: index, %result: %first as index)",
    ],
)
def test_tied_binder_requires_an_argument(text: str) -> None:
    parser, _ = _formats()
    with pytest.raises(ParseError, match="not found in args or operands"):
        parser.parse(text)


def test_projected_arguments_share_the_signature_spelling() -> None:
    parser, _ = _formats()
    module = parser.parse(
        "test.split_func @f(%condition: i1, %extent: index) {\n"
        "  scf.if %condition {\n"
        "    %local = test.constant 4 : index\n"
        "    test.use %extent : index\n"
        "  }\n"
        "  test.yield\n"
        "} launch {\n"
        "  test.use %extent : index\n"
        "  test.yield\n"
        "}\n"
    )
    config = module.body.ops[0].regions[0].blocks[0]
    nested = config.ops[0].regions[0].blocks[0]
    module.values[nested.ops[0].results[0]].name = "extent"
    loaded, _ = _roundtrip(module)
    config, body = [region.blocks[0] for region in loaded.body.ops[0].regions]
    assert config.arg_ids[1] != body.arg_ids[1]
    assert config.ops[0].regions[0].blocks[0].ops[1].operands[0] == config.arg_ids[1]
    assert body.ops[0].operands[0] == body.arg_ids[1]


def test_generated_names_reserve_authored_suffixes() -> None:
    parser, _ = _formats()
    module = parser.parse(
        "func.def @f(%condition: i1, %extent: index, %reserved: index) {\n"
        "  scf.if %condition {\n"
        "    %local = test.constant 4 : index\n"
        "    test.use %extent : index\n"
        "    test.use %reserved : index\n"
        "  }\n"
        "  func.return\n"
        "}\n"
    )
    body = module.body.ops[0].regions[0].blocks[0]
    authored_suffix = f"extent${body.arg_ids[1]}"
    module.values[body.arg_ids[2]].name = authored_suffix
    nested = body.ops[0].regions[0].blocks[0]
    module.values[nested.ops[0].results[0]].name = "extent"
    loaded, _ = _roundtrip(module)
    body = loaded.body.ops[0].regions[0].blocks[0]
    nested = body.ops[0].regions[0].blocks[0]
    assert loaded.values[body.arg_ids[2]].name == authored_suffix
    assert nested.ops[1].operands[0] == body.arg_ids[1]
    assert nested.ops[2].operands[0] == body.arg_ids[2]


def test_unnamed_argument_and_named_numeric_value_stay_distinct() -> None:
    parser, _ = _formats()
    module = parser.parse(
        "func.def @f(%extent: index) {\n"
        "  %local = test.constant 4 : index\n"
        "  test.use %extent : index\n"
        "  test.use %local : index\n"
        "  func.return\n"
        "}\n"
    )
    body = module.body.ops[0].regions[0].blocks[0]
    module.values[body.arg_ids[0]].name = ""
    module.values[body.ops[0].results[0]].name = str(body.arg_ids[0])
    loaded, _ = _roundtrip(module)
    body = loaded.body.ops[0].regions[0].blocks[0]
    assert body.ops[1].operands[0] == body.arg_ids[0]
    assert body.ops[2].operands[0] == body.ops[0].results[0]


def test_native_numeric_collision_suffix_roundtrips() -> None:
    parser, printer = _formats()
    text = (
        "func.def @f(%789$141: index) {\n"
        "  %0_1 = test.constant 4 : index\n"
        "  %value$2 = test.convert %0_1 : index -> index\n"
        "  test.use %789$141 : index\n"
        "  test.use %value$2 : index\n"
        "  func.return\n"
        "}\n"
    )
    module = parser.parse(text)
    assert printer.print_module(module) == text
    body = module.body.ops[0].regions[0].blocks[0]
    assert module.values[body.arg_ids[0]].name == "789$141"
    assert module.values[body.ops[0].results[0]].name == "0_1"
    assert module.values[body.ops[1].results[0]].name == "value$2"
    assert body.ops[2].operands[0] == body.arg_ids[0]
    assert body.ops[3].operands[0] == body.ops[1].results[0]
