# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Word-wise arithmetic rules for packed byte vectors."""

from __future__ import annotations

from dataclasses import replace
from itertools import product

from loom.dialect.vector import defs as vector
from loom.target.contracts import (
    DescriptorEmitForm,
    DescriptorRule,
    EmitDescriptorOp,
    Guard,
    GuardDiagnostic,
    ValueAliasRule,
    ValueRef,
    Vector,
    descriptor_by_key,
)
from loom.target.low_descriptors import DescriptorSet

PACKED_I8_TYPE = Vector(
    "i8",
    minimum_lanes=1,
    maximum_lanes="LOOM_AMDGPU_MAX_PACKED_I8_LANES",
)
PACKED_I8_TYPE_DIAGNOSTIC = GuardDiagnostic(
    subject_role="type",
    subject_name="vector<i8>",
    constraint_key="amdgpu.arithmetic.vector_i8_packed",
)
_TYPE_GUARDS = tuple(
    Guard.value_type(field, PACKED_I8_TYPE, diagnostic=PACKED_I8_TYPE_DIAGNOSTIC)
    for field in ("lhs", "rhs", "result")
)
_PACKED_I8_LOW7_MASK = 0x7F7F7F7F
_PACKED_I8_SIGN_MASK = 0x80808080


def _register_bank_rules(
    descriptors: DescriptorSet, rule: DescriptorRule, operands: tuple[str, ...]
) -> tuple[DescriptorRule, ...]:
    """Broadcasts scalar words only where packed VALU arithmetic requires them."""
    move = descriptor_by_key(descriptors, "amdgpu.v_mov_b32_copy")
    rules = []
    for banks in product(("amdgpu.vgpr", "amdgpu.sgpr"), repeat=len(operands)):
        replacements = {
            ValueRef.operand(operand): ValueRef.temporary(operand + "_vgpr")
            for operand, bank in zip(operands, banks, strict=True)
            if bank == "amdgpu.sgpr"
        }
        broadcasts = tuple(
            EmitDescriptorOp(
                descriptor=move,
                operands={"src": source},
                results={"dst": result},
                result_types={"dst": ValueRef.result("result")},
                form=DescriptorEmitForm.PER_LANE_SEQUENCE,
            )
            for source, result in replacements.items()
        )
        rules.append(
            replace(
                rule,
                guards=(
                    *rule.guards,
                    *(
                        Guard.low_value_register_class(operand, bank)
                        for operand, bank in zip(operands, banks, strict=True)
                    ),
                    *((Guard.descriptor_available(move),) if broadcasts else ()),
                ),
                emit=(
                    *broadcasts,
                    *(
                        replace(
                            step,
                            operands={
                                name: replacements.get(value, value)
                                for name, value in step.operands.items()
                            },
                        )
                        for step in rule.emit
                    ),
                ),
            )
        )
    return tuple(rules)


def packed_i8_add_rules(descriptors: DescriptorSet) -> tuple[DescriptorRule, ...]:
    and_literal = descriptor_by_key(descriptors, "amdgpu.v_and_b32.lit")
    add = descriptor_by_key(descriptors, "amdgpu.v_add_u32")
    xor_bits = descriptor_by_key(descriptors, "amdgpu.v_xor_b32")
    result_type = {"dst": ValueRef.result("result")}
    rule = DescriptorRule(
        source_op=vector.vector_addi,
        descriptor=add,
        guards=(
            *_TYPE_GUARDS,
            Guard.descriptor_available(and_literal),
            Guard.descriptor_available(add),
            Guard.descriptor_available(xor_bits),
        ),
        emit=(
            EmitDescriptorOp(
                descriptor=and_literal,
                operands={"rhs": ValueRef.operand("lhs")},
                results={"dst": ValueRef.temporary("lhs_low")},
                result_types=result_type,
                immediates={"imm32": _PACKED_I8_LOW7_MASK},
                form=DescriptorEmitForm.PER_LANE_SEQUENCE,
            ),
            EmitDescriptorOp(
                descriptor=and_literal,
                operands={"rhs": ValueRef.operand("rhs")},
                results={"dst": ValueRef.temporary("rhs_low")},
                result_types=result_type,
                immediates={"imm32": _PACKED_I8_LOW7_MASK},
                form=DescriptorEmitForm.PER_LANE_SEQUENCE,
            ),
            EmitDescriptorOp(
                descriptor=add,
                operands={
                    "lhs": ValueRef.temporary("lhs_low"),
                    "rhs": ValueRef.temporary("rhs_low"),
                },
                results={"dst": ValueRef.temporary("low_sum")},
                result_types=result_type,
                form=DescriptorEmitForm.PER_LANE_SEQUENCE,
            ),
            EmitDescriptorOp(
                descriptor=xor_bits,
                operands={
                    "lhs": ValueRef.operand("lhs"),
                    "rhs": ValueRef.operand("rhs"),
                },
                results={"dst": ValueRef.temporary("high_xor")},
                result_types=result_type,
                form=DescriptorEmitForm.PER_LANE_SEQUENCE,
            ),
            EmitDescriptorOp(
                descriptor=and_literal,
                operands={"rhs": ValueRef.temporary("high_xor")},
                results={"dst": ValueRef.temporary("high_bits")},
                result_types=result_type,
                immediates={"imm32": _PACKED_I8_SIGN_MASK},
                form=DescriptorEmitForm.PER_LANE_SEQUENCE,
            ),
            EmitDescriptorOp(
                descriptor=xor_bits,
                operands={
                    "lhs": ValueRef.temporary("low_sum"),
                    "rhs": ValueRef.temporary("high_bits"),
                },
                results={"dst": ValueRef.result("result")},
                form=DescriptorEmitForm.PER_LANE_SEQUENCE,
            ),
        ),
    )

    return _register_bank_rules(descriptors, rule, ("lhs", "rhs"))


def packed_i8_sub_rules(descriptors: DescriptorSet) -> tuple[DescriptorRule, ...]:
    and_literal = descriptor_by_key(descriptors, "amdgpu.v_and_b32.lit")
    or_literal = descriptor_by_key(descriptors, "amdgpu.v_or_b32.lit")
    sub = descriptor_by_key(descriptors, "amdgpu.v_sub_u32")
    xor_bits = descriptor_by_key(descriptors, "amdgpu.v_xor_b32")
    xor_literal = descriptor_by_key(descriptors, "amdgpu.v_xor_b32.lit")
    result_type = {"dst": ValueRef.result("result")}
    rule = DescriptorRule(
        source_op=vector.vector_subi,
        descriptor=sub,
        guards=(
            *_TYPE_GUARDS,
            Guard.descriptor_available(and_literal),
            Guard.descriptor_available(or_literal),
            Guard.descriptor_available(sub),
            Guard.descriptor_available(xor_bits),
            Guard.descriptor_available(xor_literal),
        ),
        emit=(
            EmitDescriptorOp(
                descriptor=or_literal,
                operands={"rhs": ValueRef.operand("lhs")},
                results={"dst": ValueRef.temporary("lhs_guard")},
                result_types=result_type,
                immediates={"imm32": _PACKED_I8_SIGN_MASK},
                form=DescriptorEmitForm.PER_LANE_SEQUENCE,
            ),
            EmitDescriptorOp(
                descriptor=and_literal,
                operands={"rhs": ValueRef.operand("rhs")},
                results={"dst": ValueRef.temporary("rhs_low")},
                result_types=result_type,
                immediates={"imm32": _PACKED_I8_LOW7_MASK},
                form=DescriptorEmitForm.PER_LANE_SEQUENCE,
            ),
            EmitDescriptorOp(
                descriptor=sub,
                operands={
                    "lhs": ValueRef.temporary("lhs_guard"),
                    "rhs": ValueRef.temporary("rhs_low"),
                },
                results={"dst": ValueRef.temporary("low_diff")},
                result_types=result_type,
                form=DescriptorEmitForm.PER_LANE_SEQUENCE,
            ),
            EmitDescriptorOp(
                descriptor=xor_bits,
                operands={
                    "lhs": ValueRef.operand("lhs"),
                    "rhs": ValueRef.operand("rhs"),
                },
                results={"dst": ValueRef.temporary("high_xor")},
                result_types=result_type,
                form=DescriptorEmitForm.PER_LANE_SEQUENCE,
            ),
            EmitDescriptorOp(
                descriptor=xor_literal,
                operands={"rhs": ValueRef.temporary("high_xor")},
                results={"dst": ValueRef.temporary("high_toggled")},
                result_types=result_type,
                immediates={"imm32": _PACKED_I8_SIGN_MASK},
                form=DescriptorEmitForm.PER_LANE_SEQUENCE,
            ),
            EmitDescriptorOp(
                descriptor=and_literal,
                operands={"rhs": ValueRef.temporary("high_toggled")},
                results={"dst": ValueRef.temporary("high_bits")},
                result_types=result_type,
                immediates={"imm32": _PACKED_I8_SIGN_MASK},
                form=DescriptorEmitForm.PER_LANE_SEQUENCE,
            ),
            EmitDescriptorOp(
                descriptor=xor_bits,
                operands={
                    "lhs": ValueRef.temporary("low_diff"),
                    "rhs": ValueRef.temporary("high_bits"),
                },
                results={"dst": ValueRef.result("result")},
                form=DescriptorEmitForm.PER_LANE_SEQUENCE,
            ),
        ),
    )

    return _register_bank_rules(descriptors, rule, ("lhs", "rhs"))


def packed_i8_logical_shift_rules(
    descriptors: DescriptorSet,
) -> tuple[DescriptorRule | ValueAliasRule, ...]:
    """Shifts bytes together and clears bits crossing byte boundaries."""
    mask_descriptor = descriptor_by_key(descriptors, "amdgpu.v_and_b32.lit")
    rules: list[DescriptorRule | ValueAliasRule] = []
    for source_op, shift_key in (
        (vector.vector_shli, "amdgpu.v_lshlrev_b32.src0_inline"),
        (vector.vector_shrui, "amdgpu.v_lshrrev_b32.src0_inline"),
    ):
        shift_descriptor = descriptor_by_key(descriptors, shift_key)
        rules.append(
            ValueAliasRule(
                source_op=source_op,
                source=ValueRef.operand("lhs"),
                result=ValueRef.result("result"),
                guards=(*_TYPE_GUARDS, Guard.value_i64_range("rhs", 0, 0)),
            )
        )
        for amount in range(1, 8):
            byte_mask = (
                (0xFF << amount) & 0xFF
                if source_op is vector.vector_shli
                else 0xFF >> amount
            )
            rule = DescriptorRule(
                source_op=source_op,
                descriptor=shift_descriptor,
                guards=(
                    *_TYPE_GUARDS,
                    # A singleton element range proves equal runtime counts;
                    # a common non-singleton range does not.
                    Guard.value_i64_range("rhs", amount, amount),
                    Guard.descriptor_available(shift_descriptor),
                    Guard.descriptor_available(mask_descriptor),
                ),
                emit=(
                    EmitDescriptorOp(
                        descriptor=shift_descriptor,
                        operands={"value": ValueRef.operand("lhs")},
                        results={"dst": ValueRef.temporary("shifted")},
                        result_types={"dst": ValueRef.result("result")},
                        immediates={"imm32": amount},
                        form=DescriptorEmitForm.PER_LANE_SEQUENCE,
                    ),
                    EmitDescriptorOp(
                        descriptor=mask_descriptor,
                        operands={"rhs": ValueRef.temporary("shifted")},
                        results={"dst": ValueRef.result("result")},
                        immediates={"imm32": byte_mask * 0x01010101},
                        form=DescriptorEmitForm.PER_LANE_SEQUENCE,
                    ),
                ),
            )
            rules.extend(_register_bank_rules(descriptors, rule, ("lhs",)))
    return tuple(rules)
