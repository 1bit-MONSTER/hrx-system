# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

from loom.dialect.index import defs as index
from loom.dialect.scalar import bitwise as scalar
from loom.target.arch.x86.contracts.integer_bits import (
    scalar_integer_bit_count_rules,
)
from loom.target.arch.x86.descriptors import (
    X86_INTEGER_CONDITION_CODES,
    X86_SCALAR_DESCRIPTOR_SET,
)
from loom.target.arch.x86.descriptors.encoding import Flag, Form, encoding
from loom.target.contracts import EmitDescriptorOp, GuardKind, Scalar, descriptor_by_key
from loom.target.low_descriptors import DescriptorFlag, OperandRole


def _descriptor(key: str):
    return descriptor_by_key(X86_SCALAR_DESCRIPTOR_SET, key)


def _result_type(rule):
    return next(
        guard.type_pattern
        for guard in rule.guards
        if guard.kind is GuardKind.VALUE_TYPE and guard.field == "result"
    )


def test_bit_scan_descriptors_cover_both_baseline_register_widths() -> None:
    for operation, opcode in (("bsf", 0x0FBC), ("bsr", 0x0FBD)):
        for bit_count in (32, 64):
            descriptor = _descriptor(f"x86.scalar.{operation}.gpr{bit_count}")
            assert descriptor.encoding_format_id == Form.UNARY_R_RM
            assert descriptor.encoding_id == encoding(
                opcode, flags=Flag.REX_W if bit_count == 64 else 0
            )
            assert tuple(operand.role for operand in descriptor.operands) == (
                OperandRole.RESULT,
                OperandRole.OPERAND,
            )
            assert {
                alternative.reg_class
                for operand in descriptor.operands
                for alternative in operand.reg_alts
            } == {f"x86.gpr{bit_count}"}
            assert descriptor.flags == (
                DescriptorFlag.DEAD_REMOVABLE,
                DescriptorFlag.SAFE_TO_SPECULATE,
            )


def test_scalar_and_index_bit_count_rules_cover_the_complete_native_family() -> None:
    rules = scalar_integer_bit_count_rules(_descriptor)
    for rule in rules:
        rule.validate(X86_SCALAR_DESCRIPTOR_SET)

    actual = {(rule.source_op, _result_type(rule)) for rule in rules}
    expected = {
        (source_op, type_pattern)
        for type_pattern, source_ops in (
            (
                Scalar("i32"),
                (scalar.scalar_ctlzi, scalar.scalar_cttzi, scalar.scalar_ctpopi),
            ),
            (
                Scalar("i64"),
                (scalar.scalar_ctlzi, scalar.scalar_cttzi, scalar.scalar_ctpopi),
            ),
            (
                Scalar("index"),
                (index.index_ctlzi, index.index_cttzi, index.index_ctpopi),
            ),
        )
        for source_op in source_ops
    }
    assert actual == expected
    assert len(rules) == len(expected)


def test_scan_recipes_repair_zero_and_population_counts_share_swar_shape() -> None:
    for rule in scalar_integer_bit_count_rules(_descriptor):
        emits = tuple(emit for emit in rule.emit if isinstance(emit, EmitDescriptorOp))
        descriptor_keys = tuple(emit.descriptor.key for emit in emits)
        bit_count = 32 if _result_type(rule) == Scalar("i32") else 64
        suffix = f"gpr{bit_count}"
        assert rule.report_key == "integer_bit_count"

        if rule.source_op.name.endswith("ctlzi"):
            assert descriptor_keys == (
                f"x86.scalar.bsr.{suffix}",
                f"x86.scalar.xor.imm.{suffix}",
                f"x86.scalar.movimm.{suffix}",
                f"x86.scalar.cmp.select.imm.{suffix}.{suffix}",
            )
        elif rule.source_op.name.endswith("cttzi"):
            assert descriptor_keys == (
                f"x86.scalar.bsf.{suffix}",
                f"x86.scalar.movimm.{suffix}",
                f"x86.scalar.cmp.select.imm.{suffix}.{suffix}",
            )
        else:
            assert not any(".bsf." in key or ".bsr." in key for key in descriptor_keys)
            assert len(emits) == (12 if bit_count == 32 else 16)
            assert descriptor_keys[-1] == f"x86.scalar.shr.imm.{suffix}"
            continue

        select = emits[-1]
        assert select.immediates == {
            "imm32": 0,
            "predicate": dict(X86_INTEGER_CONDITION_CODES)["eq"],
        }
        width = emits[-2]
        assert width.immediates == {f"imm{bit_count}": bit_count}
