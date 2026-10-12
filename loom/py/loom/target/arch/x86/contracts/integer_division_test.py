# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

from loom.dialect.index import defs as index
from loom.dialect.scalar import arithmetic as scalar_arithmetic
from loom.target.arch.x86.contracts.scalar import X86_SCALAR_CONTRACT_FRAGMENT
from loom.target.arch.x86.descriptors import X86_AVX2_DESCRIPTOR_SET
from loom.target.contracts import DescriptorRule, EmitDescriptorOp, GuardKind, Scalar
from loom.target.low_descriptors import (
    ConstraintKind,
    DescriptorFlag,
    LatencyKind,
    ModelQuality,
    OperandFlag,
    OperandRole,
)


def _value_type_guard(rule: DescriptorRule, field: str):
    return next(
        guard.type_pattern
        for guard in rule.guards
        if guard.kind == GuardKind.VALUE_TYPE and guard.field == field
    )


def test_runtime_integer_division_rules_cover_native_carrier_family() -> None:
    division_ops = {
        scalar_arithmetic.scalar_divsi,
        scalar_arithmetic.scalar_divui,
        scalar_arithmetic.scalar_remsi,
        scalar_arithmetic.scalar_remui,
        index.index_div,
        index.index_rem,
    }
    rules = tuple(
        case
        for case in X86_SCALAR_CONTRACT_FRAGMENT.cases
        if isinstance(case, DescriptorRule)
        and case.source_op in division_ops
        and case.descriptor.key.startswith(("x86.scalar.divs.", "x86.scalar.divu."))
    )
    actual = set()
    for rule in rules:
        descriptor_keys = tuple(
            emit.descriptor.key
            for emit in rule.emit
            if isinstance(emit, EmitDescriptorOp)
        )
        ranges = tuple(
            (guard.field, guard.minimum, guard.maximum)
            for guard in rule.guards
            if guard.kind is GuardKind.VALUE_I64_RANGE
        )
        actual.add(
            (
                rule.source_op,
                _value_type_guard(rule, "result"),
                rule.descriptor.key,
                ranges,
                any(".select." in key for key in descriptor_keys),
            )
        )

    expected = set()
    for type_name, bit_count in (("i32", 32), ("i64", 64)):
        result_type = Scalar(type_name)
        suffix = f"gpr{bit_count}"
        expected.update(
            (
                source_op,
                result_type,
                f"x86.scalar.{operation}.{result_name}.{suffix}",
                (),
                False,
            )
            for source_op, operation, result_name in (
                (scalar_arithmetic.scalar_divui, "divu", "quotient"),
                (scalar_arithmetic.scalar_remui, "divu", "remainder"),
                (scalar_arithmetic.scalar_divsi, "divs", "quotient"),
            )
        )
        expected.add(
            (
                scalar_arithmetic.scalar_remsi,
                result_type,
                f"x86.scalar.divs.remainder.{suffix}",
                (),
                True,
            )
        )
        expected.update(
            (
                scalar_arithmetic.scalar_remsi,
                result_type,
                f"x86.scalar.divs.remainder.{suffix}",
                (divisor_range,),
                False,
            )
            for divisor_range in (
                ("lhs", -(2 ** (bit_count - 1)) + 1, 2 ** (bit_count - 1) - 1),
                ("rhs", -(2 ** (bit_count - 1)), -2),
                ("rhs", 0, 2 ** (bit_count - 1) - 1),
            )
        )
    expected.update(
        (
            source_op,
            Scalar("index"),
            f"x86.scalar.divu.{result_name}.gpr64",
            (),
            False,
        )
        for source_op, result_name in (
            (index.index_div, "quotient"),
            (index.index_rem, "remainder"),
        )
    )
    assert actual == expected
    assert len(rules) == len(expected)

    authored_cases = tuple(X86_SCALAR_CONTRACT_FRAGMENT.cases)
    constant_ordinals = tuple(
        ordinal
        for ordinal, case in enumerate(authored_cases)
        if isinstance(case, DescriptorRule)
        and case.source_op in division_ops
        and not case.descriptor.key.startswith(("x86.scalar.divs.", "x86.scalar.divu."))
    )
    runtime_ordinals = tuple(
        ordinal for ordinal, case in enumerate(authored_cases) if case in rules
    )
    assert constant_ordinals
    assert max(constant_ordinals) < min(runtime_ordinals)


def test_runtime_divrem_rules_preserve_paired_results() -> None:
    divrem_ops = {
        scalar_arithmetic.scalar_divremsi,
        scalar_arithmetic.scalar_divremui,
        index.index_divrem,
    }
    rules = tuple(
        case
        for case in X86_SCALAR_CONTRACT_FRAGMENT.cases
        if isinstance(case, DescriptorRule) and case.source_op in divrem_ops
    )
    actual = set()
    for rule in rules:
        divide = next(
            emit
            for emit in rule.emit
            if isinstance(emit, EmitDescriptorOp) and emit.descriptor == rule.descriptor
        )
        assert tuple(divide.results) == ("quotient", "remainder")
        assert tuple(divide.result_types) == ("quotient", "remainder")
        assert {
            guard.field for guard in rule.guards if guard.kind is GuardKind.VALUE_TYPE
        } == {"lhs", "rhs", "quotient", "remainder"}
        actual.add(
            (
                rule.source_op,
                _value_type_guard(rule, "quotient"),
                rule.descriptor.key,
            )
        )

    expected = {
        (
            scalar_arithmetic.scalar_divremui,
            Scalar(type_name),
            f"x86.scalar.divu.divrem.gpr{bit_count}",
        )
        for type_name, bit_count in (("i32", 32), ("i64", 64))
    }
    expected.update(
        (
            scalar_arithmetic.scalar_divremsi,
            Scalar(type_name),
            f"x86.scalar.divs.divrem.gpr{bit_count}",
        )
        for type_name, bit_count in (("i32", 32), ("i64", 64))
    )
    expected.add(
        (
            index.index_divrem,
            Scalar("index"),
            "x86.scalar.divu.divrem.gpr64",
        )
    )
    assert actual == expected
    assert len(rules) == len(expected)


def test_native_integer_division_descriptors_own_fixed_register_contracts() -> None:
    descriptors = {
        descriptor.key: descriptor for descriptor in X86_AVX2_DESCRIPTOR_SET.descriptors
    }
    reg_classes = {
        reg_class.name: reg_class for reg_class in X86_AVX2_DESCRIPTOR_SET.reg_classes
    }
    for name, bit_count, register in (
        ("x86.eax", 32, "rax"),
        ("x86.rax", 64, "rax"),
        ("x86.edx", 32, "rdx"),
        ("x86.rdx", 64, "rdx"),
    ):
        reg_class = reg_classes[name]
        assert reg_class.alloc_unit_bits == bit_count
        assert reg_class.physical_registers == (register,)

    expected_keys = set()
    for bit_count in (32, 64):
        suffix = f"gpr{bit_count}"
        low_class = f"x86.{'e' if bit_count == 32 else 'r'}ax"
        high_class = f"x86.{'e' if bit_count == 32 else 'r'}dx"
        expected_keys.update(
            {
                f"x86.scalar.dividend.low.{suffix}",
                f"x86.scalar.dividend.zero_high.{suffix}",
                f"x86.scalar.dividend.sign_high.{suffix}",
            }
        )
        low_descriptor = descriptors[f"x86.scalar.dividend.low.{suffix}"]
        zero_high_descriptor = descriptors[f"x86.scalar.dividend.zero_high.{suffix}"]
        sign_high_descriptor = descriptors[f"x86.scalar.dividend.sign_high.{suffix}"]
        for assignment, result_class in (
            (low_descriptor, low_class),
            (zero_high_descriptor, high_class),
        ):
            assert assignment.flags == (
                DescriptorFlag.DEAD_REMOVABLE,
                DescriptorFlag.STATE_ASSIGNMENT,
            )
            assert assignment.operands[0].reg_alts[0].reg_class == result_class
            assert assignment.operands[0].flags == (
                OperandFlag.IMPLICIT,
                OperandFlag.STATE_WRITE,
            )
        assert sign_high_descriptor.flags == (DescriptorFlag.DEAD_REMOVABLE,)
        assert sign_high_descriptor.operands[0].reg_alts[0].reg_class == high_class
        assert sign_high_descriptor.operands[0].flags == (
            OperandFlag.IMPLICIT,
            OperandFlag.STATE_WRITE,
        )
        assert sign_high_descriptor.operands[1].reg_alts[0].reg_class == low_class
        assert sign_high_descriptor.operands[1].flags == (
            OperandFlag.IMPLICIT,
            OperandFlag.STATE_READ,
        )
        for operation in ("divu", "divs"):
            paired_key = f"x86.scalar.{operation}.divrem.{suffix}"
            expected_keys.add(paired_key)
            paired = descriptors[paired_key]
            assert tuple(operand.field_name for operand in paired.operands) == (
                "quotient",
                "remainder",
                "low",
                "high",
                "divisor",
            )
            assert paired.operands[0].reg_alts[0].reg_class == low_class
            assert paired.operands[1].reg_alts[0].reg_class == high_class
            for result in paired.operands[:2]:
                assert result.flags == (
                    OperandFlag.IMPLICIT,
                    OperandFlag.STATE_WRITE,
                )
            assert paired.operands[2].reg_alts[0].reg_class == low_class
            assert paired.operands[3].reg_alts[0].reg_class == high_class
            for operand in paired.operands[2:4]:
                assert operand.flags == (
                    OperandFlag.IMPLICIT,
                    OperandFlag.STATE_READ,
                )
            assert tuple(
                (
                    constraint.kind,
                    constraint.lhs_operand_index,
                    constraint.rhs_operand_index,
                )
                for constraint in paired.constraints
            ) == (
                (ConstraintKind.TIED, 0, 2),
                (ConstraintKind.DESTRUCTIVE, 0, 2),
                (ConstraintKind.TIED, 1, 3),
                (ConstraintKind.DESTRUCTIVE, 1, 3),
            )
            for result_name, result_class, overwritten_class, tied_operand in (
                ("quotient", low_class, high_class, 1),
                ("remainder", high_class, low_class, 2),
            ):
                key = f"x86.scalar.{operation}.{result_name}.{suffix}"
                expected_keys.add(key)
                descriptor = descriptors[key]
                assert tuple(operand.field_name for operand in descriptor.operands) == (
                    "dst",
                    "low",
                    "high",
                    "divisor",
                    "other_result",
                )
                assert descriptor.operands[0].reg_alts[0].reg_class == result_class
                assert descriptor.operands[0].flags == (
                    OperandFlag.IMPLICIT,
                    OperandFlag.STATE_WRITE,
                )
                assert descriptor.operands[1].reg_alts[0].reg_class == low_class
                assert descriptor.operands[1].flags == (
                    OperandFlag.IMPLICIT,
                    OperandFlag.STATE_READ,
                )
                assert descriptor.operands[2].reg_alts[0].reg_class == high_class
                assert descriptor.operands[2].flags == (
                    OperandFlag.IMPLICIT,
                    OperandFlag.STATE_READ,
                )
                other_result = descriptor.operands[4]
                assert other_result.role is OperandRole.IMPLICIT
                assert other_result.reg_alts[0].reg_class == overwritten_class
                assert other_result.flags == (
                    OperandFlag.IMPLICIT,
                    OperandFlag.STATE_WRITE,
                )
                assert tuple(
                    (
                        constraint.kind,
                        constraint.lhs_operand_index,
                        constraint.rhs_operand_index,
                    )
                    for constraint in descriptor.constraints
                ) == (
                    (ConstraintKind.TIED, 0, tied_operand),
                    (ConstraintKind.DESTRUCTIVE, 0, tied_operand),
                )

    multiply_high = descriptors["x86.scalar.mul.high.gpr64"]
    assert multiply_high.operands[0].reg_alts[0].reg_class == "x86.rdx"
    assert multiply_high.operands[0].flags == (
        OperandFlag.IMPLICIT,
        OperandFlag.STATE_WRITE,
    )
    assert multiply_high.operands[1].reg_alts[0].reg_class == "x86.rax"
    assert multiply_high.operands[1].flags == (
        OperandFlag.IMPLICIT,
        OperandFlag.STATE_READ,
    )
    assert multiply_high.operands[3].reg_alts[0].reg_class == "x86.rax"
    assert multiply_high.operands[3].flags == (
        OperandFlag.IMPLICIT,
        OperandFlag.STATE_WRITE,
    )

    actual_keys = {
        key
        for key in descriptors
        if key.startswith(
            (
                "x86.scalar.dividend.",
                "x86.scalar.divu.",
                "x86.scalar.divs.",
            )
        )
    }
    assert actual_keys == expected_keys

    schedule_classes = {
        schedule.name: schedule for schedule in X86_AVX2_DESCRIPTOR_SET.schedule_classes
    }
    for bit_count in (32, 64):
        schedule = schedule_classes[f"x86.division.gpr{bit_count}"]
        assert schedule.latency_kind is LatencyKind.VARIABLE
        assert schedule.model_quality is ModelQuality.FALLBACK
        assert tuple(issue.resource for issue in schedule.issue_uses) == (
            "x86.division",
        )
