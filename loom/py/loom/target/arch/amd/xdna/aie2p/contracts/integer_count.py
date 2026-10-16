# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Scalar and packed integer bit-count contracts for AMD XDNA AIE2P."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Literal

from loom.dialect.index import defs as index
from loom.dialect.scalar import bitwise as scalar
from loom.dialect.vector import defs as vector
from loom.dsl import Op
from loom.target.arch.amd.xdna.aie2p.contracts import core as core_rules
from loom.target.arch.amd.xdna.aie2p.contracts.packet_program import (
    PacketProgram,
    PairPacketProgram,
    shift_integer_packet_fixed,
)
from loom.target.arch.amd.xdna.aie2p.contracts.scalar_program import ScalarProgram
from loom.target.arch.amd.xdna.aie2p.contracts.selection import (
    PredicateUpdate,
    predicate_binary_emits,
)
from loom.target.arch.amd.xdna.aie2p.core_descriptors import (
    AIE2P_CORE_DESCRIPTOR_SET,
)
from loom.target.contracts import (
    ContractCase,
    DescriptorEmitForm,
    DescriptorResultType,
    DescriptorRule,
    EmitDescriptorOp,
    EmitRegisterConcat,
    EmitRegisterSlice,
    Guard,
    Scalar,
    TypePattern,
    ValueAliasRule,
    ValueRef,
    Vector,
    descriptor_by_key,
)
from loom.target.low_descriptors import Descriptor

CountKind = Literal["leading", "trailing", "population"]

_I1 = Scalar("i1")
_I64 = Scalar("i64")
_I1_VECTOR = Vector("i1", minimum_static_elements=1, maximum_static_elements=64)


@dataclass(frozen=True, slots=True)
class IntegerCountOperation:
    """One index/scalar/vector source family with the same count semantics."""

    index_op: Op
    scalar_op: Op
    vector_op: Op
    kind: CountKind


INTEGER_COUNT_OPERATIONS = (
    IntegerCountOperation(
        index.index_ctlzi,
        scalar.scalar_ctlzi,
        vector.vector_ctlzi,
        "leading",
    ),
    IntegerCountOperation(
        index.index_cttzi,
        scalar.scalar_cttzi,
        vector.vector_cttzi,
        "trailing",
    ),
    IntegerCountOperation(
        index.index_ctpopi,
        scalar.scalar_ctpopi,
        vector.vector_ctpopi,
        "population",
    ),
)

INTEGER_COUNT_VECTOR_TYPES = tuple(
    Vector(
        f"i{element_bits}",
        minimum_static_elements=1,
        maximum_static_elements=512 // element_bits,
    )
    for element_bits in (8, 16, 32, 64)
)


def _descriptor(key: str) -> Descriptor:
    return descriptor_by_key(AIE2P_CORE_DESCRIPTOR_SET, key)


def _typed_guards(
    fields: tuple[str, ...], type_pattern: TypePattern
) -> tuple[Guard, ...]:
    return tuple(Guard.value_type(field, type_pattern) for field in fields)


def _repeat_byte(byte: int, element_bits: int) -> int:
    return int.from_bytes(bytes((byte,)) * (element_bits // 8), "little")


class _PackedCountProgram:
    """One packet program with shared lane constants."""

    def __init__(self, program: PacketProgram) -> None:
        self.program = program
        self.element_bits = program.element_bits
        self.splats: dict[int, ValueRef] = {}

    def splat(self, name: str, value: int) -> ValueRef:
        result = self.splats.get(value)
        if result is None:
            result = self.program.splat(name, value)
            self.splats[value] = result
        return result

    def binary(
        self,
        name: str | None,
        operation: str,
        lhs: ValueRef,
        rhs: ValueRef,
    ) -> ValueRef:
        return self.program.operation(name, operation, "d", s1=lhs, s2=rhs)

    def shift_right(self, name: str, value: ValueRef, amount: int) -> ValueRef:
        return shift_integer_packet_fixed(
            self.program,
            name,
            value,
            -amount,
        )


def _packed_population_count(
    recipe: _PackedCountProgram,
    value: ValueRef,
    prefix: str,
    result_name: str | None,
) -> ValueRef:
    """Counts each native 8-, 16-, or 32-bit lane with a SWAR tree."""

    element_bits = recipe.element_bits
    mask1 = recipe.splat(f"{prefix}_mask1", _repeat_byte(0x55, element_bits))
    shifted1 = recipe.shift_right(f"{prefix}_shifted1", value, 1)
    masked1 = recipe.binary(f"{prefix}_masked1", "and.bits512", shifted1, mask1)
    reduced1 = recipe.binary(
        f"{prefix}_reduced1",
        f"sub.i{element_bits}x{512 // element_bits}",
        value,
        masked1,
    )

    mask2 = recipe.splat(f"{prefix}_mask2", _repeat_byte(0x33, element_bits))
    low2 = recipe.binary(f"{prefix}_low2", "and.bits512", reduced1, mask2)
    shifted2 = recipe.shift_right(f"{prefix}_shifted2", reduced1, 2)
    high2 = recipe.binary(f"{prefix}_high2", "and.bits512", shifted2, mask2)
    reduced2 = recipe.binary(
        f"{prefix}_reduced2",
        f"add.i{element_bits}x{512 // element_bits}",
        low2,
        high2,
    )

    mask4_value = _repeat_byte(0x0F, element_bits)
    mask4 = recipe.splat(f"{prefix}_mask4", mask4_value)
    shifted4 = recipe.shift_right(f"{prefix}_shifted4", reduced2, 4)
    summed4 = recipe.binary(
        f"{prefix}_summed4",
        f"add.i{element_bits}x{512 // element_bits}",
        reduced2,
        shifted4,
    )
    nibbles_name = result_name if element_bits == 8 else f"{prefix}_nibbles"
    current = recipe.binary(nibbles_name, "and.bits512", summed4, mask4)
    if element_bits == 8:
        return current

    for amount in (8, 16):
        if amount >= element_bits:
            break
        shifted = recipe.shift_right(f"{prefix}_shifted{amount}", current, amount)
        current = recipe.binary(
            f"{prefix}_summed{amount}",
            f"add.i{element_bits}x{512 // element_bits}",
            current,
            shifted,
        )
    count_mask = recipe.splat(f"{prefix}_count_mask", 2 * element_bits - 1)
    return recipe.binary(result_name, "and.bits512", current, count_mask)


def _packed_count(
    recipe: _PackedCountProgram,
    kind: CountKind,
    value: ValueRef,
    prefix: str,
    result_name: str | None,
) -> ValueRef:
    if kind == "population":
        return _packed_population_count(recipe, value, prefix, result_name)

    element_bits = recipe.element_bits
    if kind == "leading":
        current = value
        for amount in (1, 2, 4, 8, 16):
            if amount >= element_bits:
                break
            shifted = recipe.shift_right(
                f"{prefix}_propagated_shift{amount}", current, amount
            )
            current = recipe.binary(
                f"{prefix}_propagated{amount}", "or.bits512", current, shifted
            )
        population = _packed_population_count(
            recipe, current, f"{prefix}_population", f"{prefix}_set_bits"
        )
        width = recipe.splat(f"{prefix}_width", element_bits)
        return recipe.binary(
            result_name,
            f"sub.i{element_bits}x{512 // element_bits}",
            width,
            population,
        )

    zero = recipe.binary(
        f"{prefix}_zero",
        f"sub.i{element_bits}x{512 // element_bits}",
        value,
        value,
    )
    negative = recipe.binary(
        f"{prefix}_negative",
        f"sub.i{element_bits}x{512 // element_bits}",
        zero,
        value,
    )
    isolated = recipe.binary(f"{prefix}_isolated", "and.bits512", value, negative)
    one = recipe.splat(f"{prefix}_one", 1)
    below = recipe.binary(
        f"{prefix}_below",
        f"sub.i{element_bits}x{512 // element_bits}",
        isolated,
        one,
    )
    return _packed_population_count(recipe, below, prefix, result_name)


def _packed_i64_count_rule(operation: IntegerCountOperation) -> DescriptorRule:
    program = PairPacketProgram()
    recipe = _PackedCountProgram(program)
    source = ValueRef.operand("input")
    word_counts = _packed_count(
        recipe,
        operation.kind,
        source,
        "words",
        "word_counts",
    )
    low, high = program.split_i64_words(source, "input")
    low_count, high_count = program.split_i64_words(word_counts, "count")
    zero = recipe.binary("zero", "sub.i32x16", low, low)

    if operation.kind == "population":
        count = recipe.binary("count", "add.i32x16", low_count, high_count)
    else:
        word_bits = recipe.splat("word_bits", 32)
        if operation.kind == "leading":
            extended = recipe.binary(
                "low_count_extended", "add.i32x16", low_count, word_bits
            )
            condition = program.compare_zero("high_is_zero", high)
            count = program.select("count", extended, high_count, condition)
        else:
            extended = recipe.binary(
                "high_count_extended", "add.i32x16", high_count, word_bits
            )
            condition = program.compare_zero("low_is_zero", low)
            count = program.select("count", extended, low_count, condition)
    program.join_i64_words(count, zero, None)
    return DescriptorRule(
        source_op=operation.vector_op,
        descriptor=program.emits[-1].descriptor,
        guards=_typed_guards(("input", "result"), INTEGER_COUNT_VECTOR_TYPES[-1]),
        emit=tuple(program.emits),
        report_key="integer_count.packed.i64_words",
    )


def _packed_count_rule(
    operation: IntegerCountOperation, type_pattern: Vector
) -> DescriptorRule:
    element_bits = int(type_pattern.element[1:])
    if element_bits == 64:
        return _packed_i64_count_rule(operation)
    recipe = _PackedCountProgram(PacketProgram(element_bits))
    _packed_count(
        recipe,
        operation.kind,
        ValueRef.operand("input"),
        "count",
        None,
    )
    return DescriptorRule(
        source_op=operation.vector_op,
        descriptor=recipe.program.emits[-1].descriptor,
        guards=_typed_guards(("input", "result"), type_pattern),
        emit=tuple(recipe.program.emits),
        report_key="integer_count.packed.swar",
    )


def _predicate_complement_rule(
    source_op: Op, type_pattern: TypePattern
) -> DescriptorRule:
    if type_pattern == _I1:
        program = ScalarProgram()
        one = program.constant("one", 1)
        program.binary(None, "xor.i32", ValueRef.operand("input"), one)
        return DescriptorRule(
            source_op=source_op,
            descriptor=program.emits[-1].descriptor,
            guards=_typed_guards(("input", "result"), type_pattern),
            emit=tuple(program.emits),
            report_key="integer_count.predicate.complement",
        )

    low = ValueRef.temporary("ones_low")
    ones = ValueRef.temporary("ones")
    emits = (
        EmitDescriptorOp(
            descriptor=_descriptor("amd.xdna.aie2p.constant.i32.predicate.low32"),
            results={"dst": low},
            result_types={"dst": DescriptorResultType()},
            immediates={"i": -1},
            form=DescriptorEmitForm.CONST,
        ),
        EmitDescriptorOp(
            descriptor=_descriptor("amd.xdna.aie2p.predicate.complete.constant.high32"),
            operands={"storage": low},
            results={"dst": ones},
            result_types={"dst": DescriptorResultType()},
            immediates={"i": -1},
            form=DescriptorEmitForm.OP,
        ),
        *predicate_binary_emits(
            "xor",
            ValueRef.operand("input"),
            ones,
            ValueRef.result("result"),
            temporary_prefix="complement",
            update=PredicateUpdate.NONE,
        ),
    )
    return DescriptorRule(
        source_op=source_op,
        descriptor=emits[-1].descriptor,
        guards=_typed_guards(("input", "result"), type_pattern),
        emit=emits,
        report_key="integer_count.predicate.complement",
    )


def _split_scalar_i64(
    source: ValueRef,
) -> tuple[tuple[EmitRegisterSlice, EmitRegisterSlice], ValueRef, ValueRef]:
    low = ValueRef.temporary("input_low")
    high = ValueRef.temporary("input_high")
    return (
        (
            EmitRegisterSlice(source=source, result=low, unit_count=1),
            EmitRegisterSlice(
                source=source,
                result=high,
                unit_offset=1,
                unit_count=1,
            ),
        ),
        low,
        high,
    )


def _scalar_i32_count(
    program: ScalarProgram,
    kind: CountKind,
    value: ValueRef,
    prefix: str,
) -> ValueRef:
    if kind == "leading":
        return program.unary(f"{prefix}_count", "clz.i32", value)
    if kind == "population":
        return program.unary(f"{prefix}_count", "popcount.i32", value)

    all_ones = program.constant(f"{prefix}_all_ones", -1)
    inverted = program.binary(f"{prefix}_inverted", "xor.i32", value, all_ones)
    minus_one = program.add_immediate(f"{prefix}_minus_one", value, -1)
    trailing_mask = program.binary(
        f"{prefix}_trailing_mask", "and.i32", inverted, minus_one
    )
    nonzero_count = program.unary(
        f"{prefix}_nonzero_count", "popcount.i32", trailing_mask
    )
    width = program.constant(f"{prefix}_width", 32)
    return program.select(
        f"{prefix}_count",
        width,
        nonzero_count,
        value,
        when_zero=True,
    )


def _scalar_i64_count_rule(operation: IntegerCountOperation) -> DescriptorRule:
    slices, low, high = _split_scalar_i64(ValueRef.operand("input"))
    program = ScalarProgram()
    zero = program.constant("result_high", 0)
    low_count = _scalar_i32_count(program, operation.kind, low, "low")
    high_count = _scalar_i32_count(program, operation.kind, high, "high")
    if operation.kind == "population":
        count = program.binary("count", "add.i32", low_count, high_count)
    elif operation.kind == "leading":
        extended = program.add_immediate("low_count_extended", low_count, 32)
        count = program.select("count", extended, high_count, high, when_zero=True)
    else:
        extended = program.add_immediate("high_count_extended", high_count, 32)
        count = program.select("count", extended, low_count, low, when_zero=True)
    return DescriptorRule(
        source_op=operation.scalar_op,
        descriptor=program.emits[-1].descriptor,
        guards=_typed_guards(("input", "result"), _I64),
        emit=(
            *slices,
            *program.emits,
            EmitRegisterConcat(
                sources=(count, zero),
                result=ValueRef.result("result"),
            ),
        ),
        report_key="integer_count.scalar.i64_words",
    )


def _predicate_rules() -> tuple[ContractCase, ...]:
    return tuple(
        rule
        for operation in INTEGER_COUNT_OPERATIONS
        for type_pattern, source_op in (
            (_I1, operation.scalar_op),
            (_I1_VECTOR, operation.vector_op),
        )
        for rule in (
            ValueAliasRule(
                source_op=source_op,
                source=ValueRef.operand("input"),
                result=ValueRef.result("result"),
                guards=_typed_guards(("input", "result"), type_pattern),
            )
            if operation.kind == "population"
            else _predicate_complement_rule(source_op, type_pattern),
        )
    )


def _native_word_rule(
    source_op: Op,
    kind: CountKind,
    type_pattern: TypePattern,
) -> DescriptorRule:
    if kind == "leading":
        return core_rules._unary_rule(
            source_op,
            type_pattern,
            "amd.xdna.aie2p.clz.i32",
        )
    if kind == "trailing":
        return core_rules._count_trailing_zeros_rule(source_op, type_pattern)
    return core_rules._unary_rule(
        source_op,
        type_pattern,
        "amd.xdna.aie2p.popcount.i32",
    )


AIE2P_INTEGER_COUNT_RULES: tuple[ContractCase, ...] = (
    *(
        _native_word_rule(source_op, operation.kind, type_pattern)
        for operation in INTEGER_COUNT_OPERATIONS
        for source_op, type_pattern in (
            (operation.index_op, core_rules._INDEX),
            (operation.scalar_op, core_rules._I32),
        )
    ),
    *_predicate_rules(),
    *(_scalar_i64_count_rule(operation) for operation in INTEGER_COUNT_OPERATIONS),
    *(
        _packed_count_rule(operation, type_pattern)
        for operation in INTEGER_COUNT_OPERATIONS
        for type_pattern in INTEGER_COUNT_VECTOR_TYPES
    ),
)
