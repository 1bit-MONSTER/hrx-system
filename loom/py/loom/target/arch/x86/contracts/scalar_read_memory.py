# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""x86 lowering-only scalar read-memory contract fragment."""

from __future__ import annotations

from collections.abc import Iterable, Sequence

from loom.dialect.scalar import ALL_SCALAR_OPS
from loom.dialect.scalar import arithmetic as scalar_arithmetic
from loom.dialect.scalar import bitwise as scalar_bitwise
from loom.dialect.view import ALL_VIEW_OPS
from loom.dialect.view import defs as view
from loom.dsl import Op
from loom.target.arch.x86.contracts.memory import x86_fused_load_rules
from loom.target.arch.x86.descriptors import X86_SCALAR_DESCRIPTOR_SET
from loom.target.arch.x86.scalar_families import X86_SCALAR_READ_MEMORY_FAMILIES
from loom.target.contracts import (
    ContractCase,
    ContractFragment,
    DescriptorRule,
    Guard,
    GuardDiagnostic,
    Scalar,
    SourceNode,
    TypePattern,
    ValueRef,
    descriptor_by_key,
)
from loom.target.low_descriptors import Descriptor

_I32 = Scalar("i32")
_I64 = Scalar("i64")

# Two-node graphs score above the ordinary one-node load and arithmetic rules.
# Immediate arithmetic uses priority three in the scalar legality fragment and
# therefore remains preferable when both realizations are available.
_READ_MEMORY_PRIORITY = 1

_SOURCE_MEMORY_DIAGNOSTIC = GuardDiagnostic(
    subject_role="source-memory",
    subject_name="x86-scalar",
    constraint_key="x86.scalar.source_memory",
)


def _descriptor(key: str) -> Descriptor:
    return descriptor_by_key(X86_SCALAR_DESCRIPTOR_SET, key)


def _typed_guards(
    fields: Iterable[str],
    type_pattern: TypePattern,
) -> tuple[Guard, ...]:
    return tuple(Guard.value_type(field, type_pattern) for field in fields)


def _read_memory_rules(
    source_op: Op,
    type_pattern: TypePattern,
    operation: str,
    register_width: int,
    *,
    loaded_operands: Sequence[str] = ("lhs", "rhs"),
) -> Iterable[DescriptorRule]:
    for loaded_operand in loaded_operands:
        accumulator_operand = "rhs" if loaded_operand == "lhs" else "lhs"
        consumer = SourceNode.exclusive_user(
            "consumer",
            source_op=source_op,
            parent_result=ValueRef.result("result"),
            node_operand=ValueRef.operand(loaded_operand),
            guards=_typed_guards(("lhs", "rhs", "result"), type_pattern),
        )
        yield from x86_fused_load_rules(
            _descriptor,
            source_op=view.view_load,
            source_type=type_pattern,
            source_nodes=(consumer,),
            result=ValueRef.result("result", source_node="consumer"),
            element_byte_count=register_width // 8,
            lane_count=1,
            descriptor_key_prefix=f"x86.scalar.{operation}",
            register_suffix=f"gpr{register_width}",
            instruction_operands={
                "lhs": ValueRef.operand(accumulator_operand, source_node="consumer")
            },
            diagnostic=_SOURCE_MEMORY_DIAGNOSTIC,
            report_key=(
                f"native_memory_integer_{operation}_i{register_width}_{loaded_operand}"
            ),
            priority=_READ_MEMORY_PRIORITY,
        )


def _cases() -> tuple[ContractCase, ...]:
    source_operations = {
        "addi": scalar_arithmetic.scalar_addi,
        "subi": scalar_arithmetic.scalar_subi,
        "muli": scalar_arithmetic.scalar_muli,
        "andi": scalar_bitwise.scalar_andi,
        "ori": scalar_bitwise.scalar_ori,
        "xori": scalar_bitwise.scalar_xori,
    }
    return tuple(
        rule
        for type_pattern, register_width in ((_I32, 32), (_I64, 64))
        for family in X86_SCALAR_READ_MEMORY_FAMILIES
        for rule in _read_memory_rules(
            source_operations[family.source_operation],
            type_pattern,
            family.descriptor_operation,
            register_width,
            loaded_operands=(("lhs", "rhs") if family.commutative else ("rhs",)),
        )
    )


X86_SCALAR_READ_MEMORY_CONTRACT_DIALECT_OPS = {
    "scalar": ALL_SCALAR_OPS,
    "view": ALL_VIEW_OPS,
}

X86_SCALAR_READ_MEMORY_CONTRACT_FRAGMENT = ContractFragment(
    name="x86.scalar.read_memory",
    descriptor_set=X86_SCALAR_DESCRIPTOR_SET,
    public_header="loom/target/arch/x86/contracts/scalar_read_memory.h",
    cases=_cases(),
    # These are optional lowering realizations. Each source operation has an
    # independent legality contract, so exposing their overlapping graphs to
    # speculative legalization would add no supported source program.
    target_contract_query=False,
)
