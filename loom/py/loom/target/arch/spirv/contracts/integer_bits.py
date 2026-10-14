# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Width-aware integer bit recipes shared by scalar, vector and index ops."""

from loom.dialect.scalar import bitwise as scalar
from loom.dialect.vector import defs as vector
from loom.dsl import Op
from loom.target.arch.spirv.contracts.descriptor_rule import (
    IntegerRecipe,
    descriptor_feature_guards,
)
from loom.target.contracts import (
    ContractCase,
    DescriptorEmitForm,
    DescriptorRule,
    Guard,
    Scalar,
    TypePattern,
    ValueAliasRule,
    ValueRef,
    Vector,
)


def _suffix(width: int, lanes: int) -> str:
    return f"i{width}" if lanes == 1 else f"v{lanes}i{width}"


def _leading_zeros(recipe: IntegerRecipe, value: ValueRef) -> None:
    width = recipe.integer_width
    suffix = recipe.integer_suffix
    word_suffix = _suffix(32, recipe.lanes)
    word = value
    last_bit = recipe.constant(width - 1, bit_width=32)
    if width < 32:
        extended = recipe.emit(f"s_convert.{suffix}.{word_suffix}", {"input": value})
        word = recipe.emit(
            f"bitwise_and.{word_suffix}",
            {"lhs": extended, "rhs": recipe.constant((1 << width) - 1, bit_width=32)},
        )
    elif width == 64:
        # FindUMsb only accepts 32-bit components. Select the significant word
        # before scanning it, so each lane needs just one scan even for I64.
        high = recipe.binary("shift_right_logical", value, recipe.constant(32))
        low_word = recipe.emit(f"s_convert.{suffix}.{word_suffix}", {"input": value})
        high_word = recipe.emit(f"s_convert.{suffix}.{word_suffix}", {"input": high})
        high_nonzero = recipe.emit(
            f"i_not_equal.{word_suffix}",
            {"lhs": high_word, "rhs": recipe.constant(0, bit_width=32)},
        )
        word = recipe.emit(
            f"select.{word_suffix}",
            {
                "condition": high_nonzero,
                "true_value": high_word,
                "false_value": low_word,
            },
        )
        last_bit = recipe.emit(
            f"select.{word_suffix}",
            {
                "condition": high_nonzero,
                "true_value": recipe.constant(31, bit_width=32),
                "false_value": last_bit,
            },
        )
    highest_bit = recipe.emit(
        f"ext_inst.glsl_std_450.find_umsb.{word_suffix}", {"input": word}
    )
    # FindUMsb(0) is -1, giving exactly the input width for an all-zero value.
    # Keep the count arithmetic in I32, even when the source carrier is I64.
    result = ValueRef.result("result")
    count = result if width == 32 else ValueRef.temporary("word_count")
    recipe.emit(
        f"isub.{word_suffix}",
        {"lhs": last_bit, "rhs": highest_bit},
        result=count,
    )
    if width != 32:
        recipe.emit(
            f"s_convert.{word_suffix}.{suffix}", {"input": count}, result=result
        )


def bit_count_rule(
    source_op: Op, pattern: TypePattern, width: int, lanes: int = 1
) -> DescriptorRule:
    """Lowers a payload or index count, including its defined all-zero result."""
    recipe = IntegerRecipe(width, lanes)
    value = ValueRef.operand("input")
    if source_op.name.endswith("ctlzi"):
        _leading_zeros(recipe, value)
    else:
        if source_op.name.endswith("cttzi"):
            negated = recipe.binary("isub", recipe.constant(0), value)
            low_bit = recipe.binary("bitwise_and", value, negated)
            # Subtraction wraps for zero, producing an all-one mask.
            value = recipe.binary("isub", low_bit, recipe.constant(1))
        recipe.emit(
            f"bit_count.{recipe.integer_suffix}",
            {"input": value},
            result=ValueRef.result("result"),
        )
    return DescriptorRule(
        source_op=source_op,
        descriptor=recipe.emits[-1].descriptor,
        guards=(
            Guard.value_type("input", pattern),
            Guard.value_type("result", pattern),
            *descriptor_feature_guards(*(step.descriptor for step in recipe.emits)),
        ),
        emit=tuple(recipe.emits),
        report_key="integer_bit_count" if len(recipe.emits) > 1 else "",
    )


def _boolean_count_rule(
    source_op: Op, pattern: TypePattern, lanes: int
) -> ContractCase:
    guards = (
        Guard.value_type("input", pattern),
        Guard.value_type("result", pattern),
    )
    if source_op.name.endswith("ctpopi"):
        return ValueAliasRule(
            source_op=source_op,
            source=ValueRef.operand("input"),
            result=ValueRef.result("result"),
            guards=guards,
        )
    recipe = IntegerRecipe(32, lanes)
    true = recipe.emit("constant_true.bool", {}, form=DescriptorEmitForm.CONST)
    suffix = "bool" if lanes == 1 else f"v{lanes}bool"
    if lanes != 1:
        true = recipe.emit(
            f"composite_construct.{suffix}",
            {f"component{lane}": true for lane in range(lanes)},
        )
    recipe.emit(
        f"logical_not_equal.{suffix}",
        {"lhs": ValueRef.operand("input"), "rhs": true},
        result=ValueRef.result("result"),
    )
    return DescriptorRule(
        source_op=source_op,
        descriptor=recipe.emits[-1].descriptor,
        guards=guards,
        emit=tuple(recipe.emits),
    )


def bit_count_rules() -> tuple[ContractCase, ...]:
    rules: list[ContractCase] = []
    for width in (1, 8, 16, 32, 64):
        for source_lanes in (0, 1, 2, 3, 4):
            lanes = max(1, source_lanes)
            pattern = (
                Scalar(f"i{width}")
                if source_lanes == 0
                else Vector(f"i{width}", lanes=lanes)
            )
            for operation in ("ctlzi", "cttzi", "ctpopi"):
                source_op = (
                    getattr(scalar, f"scalar_{operation}")
                    if source_lanes == 0
                    else getattr(vector, f"vector_{operation}")
                )
                rules.append(
                    _boolean_count_rule(source_op, pattern, lanes)
                    if width == 1
                    else bit_count_rule(source_op, pattern, width, lanes)
                )
    return tuple(rules)
