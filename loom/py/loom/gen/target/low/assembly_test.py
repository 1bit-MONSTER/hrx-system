# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

from dataclasses import replace

import pytest

from loom.gen.target.low import compiler, views
from loom.target.low_descriptors import (
    AsmForm,
    AsmOperandSegment,
    AsmOperandSegmentDelimiter,
    NativeAsmValue,
    NativeAsmValueKind,
    OperandFlag,
    OperandRole,
)
from loom.target.test.descriptors import TEST_LOW_ADD_I32_DESCRIPTOR, TEST_LOW_CORE_DESCRIPTOR_SET


def _compile(descriptor):
    return compiler.compile_descriptor_set(replace(TEST_LOW_CORE_DESCRIPTOR_SET, descriptors=(descriptor,)))


@pytest.mark.parametrize("results", [("carry", "dst"), ("dst",), ()])
def test_results_require_complete_canonical_order(results):
    base = TEST_LOW_ADD_I32_DESCRIPTOR
    descriptor = replace(
        base,
        operands=(base.operands[0], replace(base.operands[0], field_name="carry"), *base.operands[1:]),
        asm_forms=(AsmForm(results=results, operands=("lhs", "rhs")),),
    )
    with pytest.raises(ValueError, match="results must enumerate canonical packet order"):
        _compile(descriptor)


@pytest.mark.parametrize("operands", [("rhs", "lhs"), ("lhs",), ()])
@pytest.mark.parametrize("segmented", [False, True])
def test_operands_require_complete_canonical_order(operands, segmented):
    form = AsmForm(results=("dst",), operands=operands)
    if segmented:
        form = replace(form, operands=(), operand_segments=(AsmOperandSegment(AsmOperandSegmentDelimiter.PAREN, operands),))
    with pytest.raises(ValueError, match="operands must enumerate canonical packet order"):
        _compile(replace(TEST_LOW_ADD_I32_DESCRIPTOR, asm_forms=(form,)))


@pytest.mark.parametrize("role", [OperandRole.OPERAND, OperandRole.PREDICATE, OperandRole.RESOURCE])
def test_packet_order_includes_every_input_role_but_not_implicit_state(role):
    base = TEST_LOW_ADD_I32_DESCRIPTOR
    descriptor = replace(
        base,
        operands=(
            base.operands[0],
            base.operands[1],
            replace(base.operands[1], field_name="state", role=OperandRole.IMPLICIT, flags=(OperandFlag.IMPLICIT,)),
            replace(base.operands[2], role=role),
        ),
    )
    compiled = _compile(descriptor)
    assert compiled.asm_forms[0].result_indices == (0,)
    assert compiled.asm_forms[0].operand_indices == (1, 3)


def test_segments_preserve_canonical_order_with_variadic_suffix():
    base = TEST_LOW_ADD_I32_DESCRIPTOR
    descriptor = replace(
        base,
        operands=(*base.operands[:2], replace(base.operands[2], flags=(OperandFlag.VARIADIC,))),
        asm_forms=(
            AsmForm(
                results=("dst",),
                operand_segments=(
                    AsmOperandSegment(AsmOperandSegmentDelimiter.PAREN, ("lhs",)),
                    AsmOperandSegment(AsmOperandSegmentDelimiter.SQUARE, ("rhs",)),
                ),
            ),
        ),
    )
    compiled = _compile(descriptor)
    assert compiled.asm_forms[0].operand_indices == (1, 2)
    assert [segment.has_variadic_operand for segment in compiled.asm_forms[0].operand_segments] == [False, True]


def test_native_assembly_order_is_independent_of_low_packet_order():
    base = TEST_LOW_ADD_I32_DESCRIPTOR
    form = replace(
        base.asm_forms[0],
        native_assembly_values=(
            NativeAsmValue(NativeAsmValueKind.OPERAND, field_name="rhs"),
            NativeAsmValue(NativeAsmValueKind.RESULT, field_name="dst"),
        ),
    )
    compiled = _compile(replace(base, asm_forms=(form,)))
    assert [value.index for value in compiled.asm_forms[0].native_assembly_values] == [2, 0]


def test_view_rejects_noncanonical_packet_order():
    base = TEST_LOW_ADD_I32_DESCRIPTOR
    compiled = _compile(base)
    descriptor = replace(base, asm_forms=(replace(base.asm_forms[0], operands=("rhs", "lhs")),))
    view = replace(TEST_LOW_CORE_DESCRIPTOR_SET, key="test.low.view", descriptors=(descriptor,))
    with pytest.raises(ValueError, match="operands must enumerate canonical packet order"):
        views.descriptor_set_view_for_spec(compiled, view)
