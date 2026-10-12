# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Integer division rules for x86 scalar carriers.

Constant rules describe reciprocal, one-LEA quotient-product, and high-bit
comparison forms independently of the divisor. One and powers of two are
reduced by shared source canonicalization. Runtime rules expose the fixed
accumulator inputs and results of native signed and unsigned division.
"""

from __future__ import annotations

from collections.abc import Callable
from typing import Literal

from loom.dialect.index import defs as index
from loom.dialect.scalar import arithmetic as scalar_arithmetic
from loom.dsl import Op
from loom.target.contracts import (
    DescriptorEmitForm,
    DescriptorResultType,
    DescriptorRule,
    EmitDescriptorOp,
    EmitRegisterCopy,
    Guard,
    GuardDiagnostic,
    Scalar,
    TypePattern,
    ValueProject,
    ValueRef,
)
from loom.target.low_descriptors import Descriptor

_I32 = Scalar("i32")
_I64 = Scalar("i64")
_I1 = Scalar("i1")
_INDEX = Scalar("index")
_DIVISOR_DIAGNOSTIC = GuardDiagnostic(
    subject_role="divisor",
    subject_name="unsigned-constant",
    constraint_key="x86.scalar.constant_divisor",
)
_NUMERATOR_DIAGNOSTIC = GuardDiagnostic(
    subject_role="numerator",
    subject_name="u32",
    constraint_key="x86.scalar.division_u32",
)


def _division_guards(
    type_pattern: TypePattern, minimum: int, maximum: int
) -> tuple[Guard, ...]:
    return (
        *(Guard.value_type(field, type_pattern) for field in ("lhs", "rhs", "result")),
        *(
            (
                Guard.value_unsigned_bit_count(
                    "lhs", 32, diagnostic=_NUMERATOR_DIAGNOSTIC
                ),
            )
            if type_pattern == _INDEX
            else ()
        ),
        Guard.value_exact_i64("rhs", diagnostic=_DIVISOR_DIAGNOSTIC),
        Guard.value_i64_range("rhs", minimum, maximum, diagnostic=_DIVISOR_DIAGNOSTIC),
    )


def _reciprocal_rule(
    source_op: Op,
    type_pattern: TypePattern,
    operation: Literal["quotient", "remainder", "lea_remainder"],
    descriptor_lookup: Callable[[str], Descriptor],
) -> DescriptorRule:
    """Emits a quotient, direct remainder, or one-LEA reconstructed remainder."""
    register_class = "gpr32" if type_pattern == _I32 else "gpr64"
    numerator = ValueRef.operand("lhs")
    emits = []
    if operation == "lea_remainder":
        # Both the reciprocal product and final subtraction consume n. Keep
        # their dependency on one copy so a fixed input can be released before
        # a scheduler prepares the destructive, potentially aliasing result.
        numerator = ValueRef.temporary("preserved")
        emits.append(EmitRegisterCopy(ValueRef.operand("lhs"), numerator, type_pattern))
    preserved = numerator
    if type_pattern == _I32:
        numerator = ValueRef.temporary("numerator")
        emits.append(
            EmitDescriptorOp(
                descriptor=descriptor_lookup("x86.scalar.movzx.gpr64.gpr32"),
                operands={"src": preserved},
                results={"dst": numerator},
                result_types={"dst": _I64},
            )
        )
    emits.append(
        EmitDescriptorOp(
            descriptor=descriptor_lookup("x86.scalar.movimm.gpr64"),
            results={"dst": ValueRef.temporary("magic")},
            result_types={"dst": _I64},
            immediates={
                "imm64": ValueProject.u32_divisor_magic_multiplier(
                    "lhs", "rhs", bit_width=32 if operation == "lea_remainder" else 64
                )
            },
            form=DescriptorEmitForm.CONST,
        )
    )
    multiplicand = numerator
    multiplier = ValueRef.temporary("magic")
    if operation != "quotient":
        multiplicand = ValueRef.temporary("low_product")
        emits.append(
            EmitDescriptorOp(
                descriptor=descriptor_lookup("x86.scalar.imul.gpr64"),
                operands={"lhs": numerator, "rhs": multiplier},
                results={"dst": multiplicand},
                result_types={"dst": _I64},
            )
        )
    guards = _division_guards(type_pattern, 2, 2**31 - 1)
    if operation == "lea_remainder":
        # Address scales are powers of two in [1, 8]; base + index * scale
        # forms q*d in one instruction. No divisor identities are encoded.
        guards = (
            *_division_guards(type_pattern, 2, 9),
            Guard.value_exact_power_of_two_i64("rhs", addend=-1),
        )
        emits.extend(
            (
                EmitDescriptorOp(
                    descriptor=descriptor_lookup("x86.scalar.shr.imm.gpr64"),
                    operands={"lhs": multiplicand},
                    results={"dst": ValueRef.temporary("quotient")},
                    result_types={"dst": _I64},
                    immediates={
                        "shift": ValueProject.u32_divisor_magic_shift(
                            "lhs", "rhs", product_bit_width=64
                        )
                    },
                ),
                EmitDescriptorOp(
                    descriptor=descriptor_lookup(
                        f"x86.scalar.lea.add_scale.{register_class}"
                    ),
                    operands={
                        "base": ValueRef.temporary("quotient"),
                        "index": ValueRef.temporary("quotient"),
                    },
                    results={"dst": ValueRef.temporary("quotient_product")},
                    result_types={"dst": type_pattern},
                    immediates={
                        "disp32": 0,
                        "scale": ValueProject.exact_i64_minus_one("rhs"),
                    },
                ),
            )
        )
        result = ValueRef.temporary("quotient_product")
    else:
        if operation == "remainder":
            multiplier = ValueRef.temporary("divisor")
            emits.append(
                EmitDescriptorOp(
                    descriptor=descriptor_lookup("x86.scalar.movimm.gpr64"),
                    results={"dst": multiplier},
                    result_types={"dst": _I64},
                    immediates={"imm64": ValueProject.exact_i64("rhs")},
                    form=DescriptorEmitForm.CONST,
                )
            )
        emits.append(
            EmitDescriptorOp(
                descriptor=descriptor_lookup("x86.scalar.mul.high.gpr64"),
                operands={"lhs": multiplicand, "rhs": multiplier},
                results={"dst": ValueRef.temporary("high")},
                result_types={"dst": DescriptorResultType()},
                copy_operands=("lhs",),
            )
        )
        result = ValueRef.temporary("high")
        if type_pattern == _I32:
            result = ValueRef.temporary("wide_result")
            emits.append(EmitRegisterCopy(ValueRef.temporary("high"), result, _I64))
    if operation == "lea_remainder":
        emits.append(
            EmitDescriptorOp(
                descriptor=descriptor_lookup(f"x86.scalar.sub.{register_class}"),
                operands={"lhs": preserved, "rhs": result},
                results={"dst": ValueRef.result("result")},
            )
        )
    elif type_pattern == _I32:
        emits.append(
            EmitDescriptorOp(
                descriptor=descriptor_lookup("x86.scalar.mov.trunc.gpr32.gpr64"),
                operands={"src": result},
                results={"dst": ValueRef.result("result")},
                result_types={"dst": type_pattern},
            )
        )
    else:
        emits.append(EmitRegisterCopy(result, ValueRef.result("result"), type_pattern))
    return DescriptorRule(
        source_op=source_op,
        descriptor=descriptor_lookup(
            "x86.scalar.imul.gpr64"
            if operation == "lea_remainder"
            else "x86.scalar.mul.high.gpr64"
        ),
        guards=guards,
        emit=tuple(emits),
    )


def _high_bit_rule(
    source_op: Op,
    type_pattern: TypePattern,
    operation: Literal["quotient", "remainder"],
    descriptor_lookup: Callable[[str], Descriptor],
) -> DescriptorRule:
    descriptor = descriptor_lookup(
        "x86.scalar.cmp.uge.imm.gpr32"
        if operation == "quotient"
        else "x86.scalar.sub.if_uge.imm.gpr32"
    )
    numerator = ValueRef.operand("lhs")
    result = ValueRef.result("result")
    emits = []
    if type_pattern == _INDEX:
        numerator = ValueRef.temporary("numerator")
        result = ValueRef.temporary("word_result")
        emits.append(
            EmitDescriptorOp(
                descriptor=descriptor_lookup("x86.scalar.mov.trunc.gpr32.gpr64"),
                operands={"src": ValueRef.operand("lhs")},
                results={"dst": numerator},
                result_types={"dst": _I32},
            )
        )
    emits.append(
        EmitDescriptorOp(
            descriptor=descriptor,
            operands={"lhs": numerator},
            results={"dst": result},
            result_types={"dst": _I32},
            immediates={"imm32": ValueProject.exact_i64_i32_word("rhs", word_index=0)},
            copy_operands=("lhs",) if operation == "remainder" else (),
        )
    )
    if type_pattern == _INDEX:
        emits.append(
            EmitDescriptorOp(
                descriptor=descriptor_lookup("x86.scalar.movzx.gpr64.gpr32"),
                operands={"src": result},
                results={"dst": ValueRef.result("result")},
                result_types={"dst": type_pattern},
            )
        )
    return DescriptorRule(
        source_op=source_op,
        descriptor=descriptor,
        guards=_division_guards(
            type_pattern,
            -(2**31) if type_pattern == _I32 else 2**31,
            -1 if type_pattern == _I32 else 2**32 - 1,
        ),
        emit=tuple(emits),
    )


def unsigned_constant_division_rules(
    descriptor_lookup: Callable[[str], Descriptor],
) -> tuple[DescriptorRule, ...]:
    return tuple(
        rule
        for quotient_op, remainder_op, type_pattern in (
            (scalar_arithmetic.scalar_divui, scalar_arithmetic.scalar_remui, _I32),
            (index.index_div, index.index_rem, _INDEX),
        )
        for rule in (
            _reciprocal_rule(
                remainder_op, type_pattern, "lea_remainder", descriptor_lookup
            ),
            _reciprocal_rule(quotient_op, type_pattern, "quotient", descriptor_lookup),
            _reciprocal_rule(
                remainder_op, type_pattern, "remainder", descriptor_lookup
            ),
            _high_bit_rule(quotient_op, type_pattern, "quotient", descriptor_lookup),
            _high_bit_rule(remainder_op, type_pattern, "remainder", descriptor_lookup),
        )
    )


def _runtime_division_rule(
    source_op: Op,
    type_pattern: TypePattern,
    bit_count: int,
    *,
    signed: bool,
    remainder: bool,
    sanitize_minus_one: bool,
    descriptor_lookup: Callable[[str], Descriptor],
    dividend_range: tuple[int, int] | None = None,
    divisor_range: tuple[int, int] | None = None,
) -> DescriptorRule:
    operation = "divs" if signed else "divu"
    result_name = "remainder" if remainder else "quotient"
    suffix = f"gpr{bit_count}"
    divide = descriptor_lookup(f"x86.scalar.{operation}.{result_name}.{suffix}")
    guards = [
        Guard.value_type(field, type_pattern) for field in ("lhs", "rhs", "result")
    ]
    if dividend_range is not None:
        guards.append(Guard.value_i64_range("lhs", *dividend_range))
    if divisor_range is not None:
        guards.append(Guard.value_i64_range("rhs", *divisor_range))

    emits: list[EmitDescriptorOp | EmitRegisterCopy] = []
    divisor = ValueRef.operand("rhs")
    if sanitize_minus_one:
        compare = descriptor_lookup(f"x86.scalar.cmp.eq.imm.{suffix}")
        constant = descriptor_lookup(f"x86.scalar.movimm.{suffix}")
        select = descriptor_lookup(f"x86.scalar.select.{suffix}")
        emits.extend(
            (
                EmitDescriptorOp(
                    descriptor=compare,
                    operands={"lhs": divisor},
                    results={"dst": ValueRef.temporary("divisor_is_minus_one")},
                    result_types={"dst": _I1},
                    immediates={"imm32": -1},
                ),
                EmitDescriptorOp(
                    descriptor=constant,
                    results={"dst": ValueRef.temporary("one")},
                    result_types={"dst": type_pattern},
                    immediates={f"imm{bit_count}": 1},
                    form=DescriptorEmitForm.CONST,
                ),
                EmitDescriptorOp(
                    descriptor=select,
                    operands={
                        "condition": ValueRef.temporary("divisor_is_minus_one"),
                        "true_value": ValueRef.temporary("one"),
                        "false_value": divisor,
                    },
                    results={"dst": ValueRef.temporary("safe_divisor")},
                    result_types={"dst": type_pattern},
                ),
            )
        )
        divisor = ValueRef.temporary("safe_divisor")

    low = descriptor_lookup(f"x86.scalar.dividend.low.{suffix}")
    high = descriptor_lookup(
        f"x86.scalar.dividend.{'sign_high' if signed else 'zero_high'}.{suffix}"
    )
    emits.extend(
        (
            EmitDescriptorOp(
                descriptor=low,
                operands={"src": ValueRef.operand("lhs")},
                results={"dst": ValueRef.temporary("dividend_low")},
                result_types={"dst": DescriptorResultType()},
            ),
            EmitDescriptorOp(
                descriptor=high,
                operands=(
                    {"low": ValueRef.temporary("dividend_low")} if signed else {}
                ),
                results={"dst": ValueRef.temporary("dividend_high")},
                result_types={"dst": DescriptorResultType()},
            ),
            EmitDescriptorOp(
                descriptor=divide,
                operands={
                    "low": ValueRef.temporary("dividend_low"),
                    "high": ValueRef.temporary("dividend_high"),
                    "divisor": divisor,
                },
                results={"dst": ValueRef.temporary("fixed_result")},
                result_types={"dst": DescriptorResultType()},
            ),
            EmitRegisterCopy(
                ValueRef.temporary("fixed_result"),
                ValueRef.result("result"),
                type_pattern,
            ),
        )
    )
    return DescriptorRule(
        source_op=source_op,
        descriptor=divide,
        guards=tuple(guards),
        emit=tuple(emits),
    )


def _runtime_divrem_rule(
    source_op: Op,
    type_pattern: TypePattern,
    bit_count: int,
    *,
    signed: bool,
    descriptor_lookup: Callable[[str], Descriptor],
) -> DescriptorRule:
    operation = "divs" if signed else "divu"
    suffix = f"gpr{bit_count}"
    divide = descriptor_lookup(f"x86.scalar.{operation}.divrem.{suffix}")
    low = descriptor_lookup(f"x86.scalar.dividend.low.{suffix}")
    high = descriptor_lookup(
        f"x86.scalar.dividend.{'sign_high' if signed else 'zero_high'}.{suffix}"
    )
    return DescriptorRule(
        source_op=source_op,
        descriptor=divide,
        guards=tuple(
            Guard.value_type(field, type_pattern)
            for field in ("lhs", "rhs", "quotient", "remainder")
        ),
        emit=(
            EmitDescriptorOp(
                descriptor=low,
                operands={"src": ValueRef.operand("lhs")},
                results={"dst": ValueRef.temporary("dividend_low")},
                result_types={"dst": DescriptorResultType()},
            ),
            EmitDescriptorOp(
                descriptor=high,
                operands=(
                    {"low": ValueRef.temporary("dividend_low")} if signed else {}
                ),
                results={"dst": ValueRef.temporary("dividend_high")},
                result_types={"dst": DescriptorResultType()},
            ),
            EmitDescriptorOp(
                descriptor=divide,
                operands={
                    "low": ValueRef.temporary("dividend_low"),
                    "high": ValueRef.temporary("dividend_high"),
                    "divisor": ValueRef.operand("rhs"),
                },
                results={
                    "quotient": ValueRef.temporary("fixed_quotient"),
                    "remainder": ValueRef.temporary("fixed_remainder"),
                },
                result_types={
                    "quotient": DescriptorResultType(),
                    "remainder": DescriptorResultType(),
                },
            ),
            EmitRegisterCopy(
                ValueRef.temporary("fixed_quotient"),
                ValueRef.result("quotient"),
                type_pattern,
            ),
            EmitRegisterCopy(
                ValueRef.temporary("fixed_remainder"),
                ValueRef.result("remainder"),
                type_pattern,
            ),
        ),
    )


def runtime_integer_division_rules(
    descriptor_lookup: Callable[[str], Descriptor],
) -> tuple[DescriptorRule, ...]:
    """Returns native dynamic quotient and remainder rules for scalar carriers."""
    rules: list[DescriptorRule] = []
    for type_pattern, bit_count in ((_I32, 32), (_I64, 64)):
        for signed, quotient_op, remainder_op in (
            (
                False,
                scalar_arithmetic.scalar_divui,
                scalar_arithmetic.scalar_remui,
            ),
            (
                True,
                scalar_arithmetic.scalar_divsi,
                scalar_arithmetic.scalar_remsi,
            ),
        ):
            rules.append(
                _runtime_divrem_rule(
                    scalar_arithmetic.scalar_divremsi
                    if signed
                    else scalar_arithmetic.scalar_divremui,
                    type_pattern,
                    bit_count,
                    signed=signed,
                    descriptor_lookup=descriptor_lookup,
                )
            )
            rules.append(
                _runtime_division_rule(
                    quotient_op,
                    type_pattern,
                    bit_count,
                    signed=signed,
                    remainder=False,
                    sanitize_minus_one=False,
                    descriptor_lookup=descriptor_lookup,
                )
            )
            if signed:
                # IDIV traps on MIN / -1 even when only the representable zero
                # remainder is observed. Fact-proven ranges avoid the guard;
                # the general rule substitutes one for the equivalent
                # remainder divisor while leaving zero divisors untouched.
                minimum = -(2 ** (bit_count - 1))
                maximum = 2 ** (bit_count - 1) - 1
                for dividend_range, divisor_range in (
                    ((minimum + 1, maximum), None),
                    (None, (minimum, -2)),
                    (None, (0, maximum)),
                ):
                    rules.append(
                        _runtime_division_rule(
                            remainder_op,
                            type_pattern,
                            bit_count,
                            signed=True,
                            remainder=True,
                            sanitize_minus_one=False,
                            descriptor_lookup=descriptor_lookup,
                            dividend_range=dividend_range,
                            divisor_range=divisor_range,
                        )
                    )
            rules.append(
                _runtime_division_rule(
                    remainder_op,
                    type_pattern,
                    bit_count,
                    signed=signed,
                    remainder=True,
                    sanitize_minus_one=signed,
                    descriptor_lookup=descriptor_lookup,
                )
            )

    for source_op, remainder in (
        (index.index_div, False),
        (index.index_rem, True),
    ):
        rules.append(
            _runtime_division_rule(
                source_op,
                _INDEX,
                64,
                signed=False,
                remainder=remainder,
                sanitize_minus_one=False,
                descriptor_lookup=descriptor_lookup,
            )
        )
    rules.append(
        _runtime_divrem_rule(
            index.index_divrem,
            _INDEX,
            64,
            signed=False,
            descriptor_lookup=descriptor_lookup,
        )
    )
    return tuple(rules)
