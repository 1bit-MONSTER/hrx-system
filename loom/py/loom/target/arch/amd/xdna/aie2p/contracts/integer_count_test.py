# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Family and semantic tests for AMD XDNA AIE2P integer bit counts."""

from __future__ import annotations

from loom.target.arch.amd.xdna.aie2p.contracts.integer_count import (
    AIE2P_INTEGER_COUNT_RULES,
    INTEGER_COUNT_OPERATIONS,
    INTEGER_COUNT_VECTOR_TYPES,
    CountKind,
    IntegerCountOperation,
)
from loom.target.contracts import (
    ContractCase,
    DescriptorEmitForm,
    DescriptorRule,
    EmitDescriptorOp,
    EmitRegisterConcat,
    EmitRegisterSlice,
    ValueAliasRule,
    ValueRef,
)

ScalarOrVector = int | tuple[int, ...]


def _mask(element_bits: int) -> int:
    return (1 << element_bits) - 1


def _result_element(rule: ContractCase) -> str:
    result_guards = [
        guard
        for guard in rule.guards
        if guard.field == "result" and guard.type_pattern is not None
    ]
    assert len(result_guards) == 1
    return result_guards[0].type_pattern.element


def _rule(source_op: object, element: str) -> ContractCase:
    rules = [
        rule
        for rule in AIE2P_INTEGER_COUNT_RULES
        if rule.source_op is source_op and _result_element(rule) == element
    ]
    assert len(rules) == 1
    return rules[0]


def _vector_rule(operation: IntegerCountOperation, element_bits: int) -> DescriptorRule:
    rule = _rule(operation.vector_op, f"i{element_bits}")
    assert isinstance(rule, DescriptorRule)
    return rule


def _descriptor_element_bits(key: str) -> int:
    for element_bits in (8, 16, 32):
        if f"i{element_bits}x" in key:
            return element_bits
    raise AssertionError(f"descriptor has no integer packet shape: {key}")


def _evaluate_vector_rule(
    rule: DescriptorRule,
    input_words: tuple[int, ...],
) -> tuple[int, ...]:
    values: dict[ValueRef, ScalarOrVector] = {
        ValueRef.operand("input"): input_words,
    }
    state: dict[str, int] = {}
    for emit in rule.emit:
        assert isinstance(emit, EmitDescriptorOp)
        key = emit.descriptor.key.removeprefix("amd.xdna.aie2p.")
        if key.startswith("state."):
            state[key.removeprefix("state.").removesuffix(".immediate")] = (
                emit.immediates["i"]
            )
            continue

        operands = {name: values[ref] for name, ref in emit.operands.items()}
        if emit.form is DescriptorEmitForm.CONST:
            result: ScalarOrVector = emit.immediates["i"]
            assert isinstance(result, int)
        elif key.startswith("splat."):
            source = operands["src"]
            assert isinstance(source, int)
            element_bits = _descriptor_element_bits(key)
            result = (source & _mask(element_bits),) * (512 // element_bits)
        elif key in ("and.bits512", "or.bits512"):
            lhs = operands["s1"]
            rhs = operands["s2"]
            assert isinstance(lhs, tuple)
            assert isinstance(rhs, tuple)
            operation = int.__and__ if key.startswith("and.") else int.__or__
            result = tuple(operation(a, b) for a, b in zip(lhs, rhs, strict=True))
        elif key.startswith(("add.i", "sub.i")):
            lhs = operands["s1"]
            rhs = operands["s2"]
            assert isinstance(lhs, tuple)
            assert isinstance(rhs, tuple)
            element_bits = _descriptor_element_bits(key)
            operation = int.__add__ if key.startswith("add.") else int.__sub__
            result = tuple(
                operation(a, b) & _mask(element_bits)
                for a, b in zip(lhs, rhs, strict=True)
            )
        elif key.startswith("widen."):
            source = operands["src"]
            shift = operands["su"]
            assert isinstance(source, tuple)
            assert isinstance(shift, int)
            assert state["saturation"] == 0
            result = tuple(value << shift for value in source)
        elif key.startswith("narrow."):
            source = operands["src"]
            shift = operands["su"]
            assert isinstance(source, tuple)
            assert isinstance(shift, int)
            assert state["saturation"] == 0
            assert state["rounding"] == 0
            element_bits = 8 if ".4x." in key else 16 if state["srs-mode"] == 0 else 32
            result = tuple((value >> shift) & _mask(element_bits) for value in source)
        elif key.startswith("cmp.eqz.i32x16"):
            source = operands["s2"]
            assert isinstance(source, tuple)
            result = sum((value == 0) << index for index, value in enumerate(source))
        elif key == "predicate.complete.zero.high32":
            result = operands["storage"]
            assert isinstance(result, int)
        elif key.startswith("select.i32x16"):
            false_values = operands["s1"]
            true_values = operands["s2"]
            predicate = operands["sel"]
            assert isinstance(false_values, tuple)
            assert isinstance(true_values, tuple)
            assert isinstance(predicate, int)
            result = tuple(
                true_value if predicate & (1 << index) else false_value
                for index, (true_value, false_value) in enumerate(
                    zip(true_values, false_values, strict=True)
                )
            )
        elif key == "shuffle.x.configured":
            first = operands["s1"]
            second = operands["s2"]
            control = operands["mod"]
            assert isinstance(first, tuple)
            assert isinstance(second, tuple)
            assert isinstance(control, int)
            if control in (4, 5):
                selected = first[(control - 4) :: 2]
                result = (*selected, *selected)
            else:
                assert control == 16
                result = tuple(
                    word
                    for pair in zip(first[:8], second[:8], strict=True)
                    for word in pair
                )
        else:
            raise AssertionError(f"unmodeled integer-count descriptor {key}")

        assert len(emit.results) == 1
        values[next(iter(emit.results.values()))] = result

    result = values[ValueRef.result("result")]
    assert isinstance(result, tuple)
    return result


def _evaluate_scalar_i64_rule(rule: DescriptorRule, input_value: int) -> int:
    values: dict[ValueRef, int | tuple[int, int]] = {
        ValueRef.operand("input"): (
            input_value & _mask(32),
            (input_value >> 32) & _mask(32),
        ),
    }
    for emit in rule.emit:
        if isinstance(emit, EmitRegisterSlice):
            source = values[emit.source]
            assert isinstance(source, tuple)
            values[emit.result] = source[emit.unit_offset]
            continue
        if isinstance(emit, EmitRegisterConcat):
            low = values[emit.sources[0]]
            high = values[emit.sources[1]]
            assert isinstance(low, int)
            assert isinstance(high, int)
            values[emit.result] = (low & _mask(32), high & _mask(32))
            continue

        assert isinstance(emit, EmitDescriptorOp)
        operands = {name: values[ref] for name, ref in emit.operands.items()}
        assert all(isinstance(value, int) for value in operands.values())
        key = emit.descriptor.key.removeprefix("amd.xdna.aie2p.")
        if emit.form is DescriptorEmitForm.CONST:
            result = emit.immediates["i"]
            assert isinstance(result, int)
        elif key == "clz.i32":
            value = operands["s0"]
            assert isinstance(value, int)
            result = 32 - value.bit_length()
        elif key == "popcount.i32":
            value = operands["s0"]
            assert isinstance(value, int)
            result = value.bit_count()
        elif key == "xor.i32":
            result = operands["s0"] ^ operands["s1"]
        elif key == "and.i32":
            result = operands["s0"] & operands["s1"]
        elif key == "add.i32":
            result = operands["s0"] + operands["s1"]
        elif key == "add.i32.immediate":
            result = operands["s0"] + emit.immediates["imm"]
        elif key == "select.zero.i32":
            result = operands["s0"] if operands["s2"] == 0 else operands["s1"]
        else:
            raise AssertionError(f"unmodeled scalar integer-count descriptor {key}")
        assert len(emit.results) == 1
        values[next(iter(emit.results.values()))] = result & _mask(32)

    result_words = values[ValueRef.result("result")]
    assert isinstance(result_words, tuple)
    return result_words[0] | (result_words[1] << 32)


def _pack(values: tuple[int, ...], element_bits: int) -> tuple[int, ...]:
    if element_bits != 64:
        return tuple(value & _mask(element_bits) for value in values)
    return tuple(
        word
        for value in values
        for word in (value & _mask(32), (value >> 32) & _mask(32))
    )


def _unpack(values: tuple[int, ...], element_bits: int) -> tuple[int, ...]:
    if element_bits != 64:
        return tuple(value & _mask(element_bits) for value in values)
    return tuple(
        values[index] | (values[index + 1] << 32) for index in range(0, len(values), 2)
    )


def _reference(kind: CountKind, value: int, element_bits: int) -> int:
    value &= _mask(element_bits)
    if kind == "leading":
        return element_bits - value.bit_length()
    if kind == "trailing":
        return element_bits if value == 0 else (value & -value).bit_length() - 1
    return value.bit_count()


def _boundary_values(element_bits: int) -> tuple[int, ...]:
    mask = _mask(element_bits)
    alternating = int.from_bytes(bytes((0x55,)) * (element_bits // 8), "little")
    values = [
        0,
        1,
        2,
        3,
        1 << (element_bits - 1),
        (1 << (element_bits - 1)) - 1,
        mask - 1,
        mask,
        alternating,
        mask ^ alternating,
    ]
    for position in (3, 4, 7, 8, 15, 16, 31, 32, 63):
        if position >= element_bits:
            break
        power = 1 << position
        values.extend((power - 1, power, power + 1, mask ^ power))
    return tuple(dict.fromkeys(value & mask for value in values))


def test_integer_count_rules_cover_every_source_family() -> None:
    assert len(AIE2P_INTEGER_COUNT_RULES) == 27
    assert [vector_type.element for vector_type in INTEGER_COUNT_VECTOR_TYPES] == [
        "i8",
        "i16",
        "i32",
        "i64",
    ]
    for vector_type in INTEGER_COUNT_VECTOR_TYPES:
        assert vector_type.minimum_static_elements == 1
        assert vector_type.maximum_static_elements == 512 // int(
            vector_type.element[1:]
        )

    for operation in INTEGER_COUNT_OPERATIONS:
        assert isinstance(_rule(operation.index_op, "index"), DescriptorRule)
        for element in ("i1", "i32", "i64"):
            scalar_rule = _rule(operation.scalar_op, element)
            if element == "i1" and operation.kind == "population":
                assert isinstance(scalar_rule, ValueAliasRule)
            else:
                assert isinstance(scalar_rule, DescriptorRule)
        for element in ("i1", "i8", "i16", "i32", "i64"):
            vector_rule = _rule(operation.vector_op, element)
            if element == "i1" and operation.kind == "population":
                assert isinstance(vector_rule, ValueAliasRule)
            else:
                assert isinstance(vector_rule, DescriptorRule)


def test_integer_count_recipes_remain_packet_bounded() -> None:
    for operation in INTEGER_COUNT_OPERATIONS:
        scalar_i64_rule = _rule(operation.scalar_op, "i64")
        assert isinstance(scalar_i64_rule, DescriptorRule)
        assert (
            sum(isinstance(emit, EmitRegisterSlice) for emit in scalar_i64_rule.emit)
            == 2
        )
        assert (
            sum(isinstance(emit, EmitRegisterConcat) for emit in scalar_i64_rule.emit)
            == 1
        )

        for element_bits in (8, 16, 32, 64):
            rule = _vector_rule(operation, element_bits)
            assert all(isinstance(emit, EmitDescriptorOp) for emit in rule.emit)
            keys = tuple(emit.descriptor.key for emit in rule.emit)
            assert not any(".extract." in key or ".insert." in key for key in keys)
            assert len(rule.emit) <= 80, (
                operation.kind,
                element_bits,
                len(rule.emit),
            )


def test_packed_integer_count_recipes_match_boundary_oracle() -> None:
    for operation in INTEGER_COUNT_OPERATIONS:
        for element_bits in (8, 16, 32, 64):
            lane_count = 512 // element_bits
            boundaries = _boundary_values(element_bits)
            for start in range(0, len(boundaries), lane_count):
                logical_values = tuple(
                    boundaries[(start + lane) % len(boundaries)]
                    for lane in range(lane_count)
                )
                actual = _unpack(
                    _evaluate_vector_rule(
                        _vector_rule(operation, element_bits),
                        _pack(logical_values, element_bits),
                    ),
                    element_bits,
                )
                expected = tuple(
                    _reference(operation.kind, value, element_bits)
                    for value in logical_values
                )
                assert actual == expected


def test_scalar_i64_count_recipes_match_word_boundaries() -> None:
    for operation in INTEGER_COUNT_OPERATIONS:
        rule = _rule(operation.scalar_op, "i64")
        assert isinstance(rule, DescriptorRule)
        for value in _boundary_values(64):
            assert _evaluate_scalar_i64_rule(rule, value) == _reference(
                operation.kind, value, 64
            )


def test_predicate_counts_use_identity_or_one_bit_complement() -> None:
    for operation in INTEGER_COUNT_OPERATIONS:
        scalar_rule = _rule(operation.scalar_op, "i1")
        vector_rule = _rule(operation.vector_op, "i1")
        if operation.kind == "population":
            assert isinstance(scalar_rule, ValueAliasRule)
            assert isinstance(vector_rule, ValueAliasRule)
            continue
        assert isinstance(scalar_rule, DescriptorRule)
        assert [emit.descriptor.key for emit in scalar_rule.emit] == [
            "amd.xdna.aie2p.constant.i32.short",
            "amd.xdna.aie2p.xor.i32",
        ]
        assert isinstance(vector_rule, DescriptorRule)
        assert [emit.descriptor.key for emit in vector_rule.emit] == [
            "amd.xdna.aie2p.constant.i32.predicate.low32",
            "amd.xdna.aie2p.predicate.complete.constant.high32",
            "amd.xdna.aie2p.predicate.xor.low32",
            "amd.xdna.aie2p.predicate.xor.high32",
        ]
