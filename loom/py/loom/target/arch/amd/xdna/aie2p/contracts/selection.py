# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Shared AIE2P whole-carrier and packed-predicate selection recipes."""

from enum import Enum

from loom.target.arch.amd.xdna.aie2p.core_descriptors import (
    AIE2P_CORE_DESCRIPTOR_SET,
)
from loom.target.contracts import (
    DescriptorEmitForm,
    DescriptorResultType,
    EmitDescriptorOp,
    ResultTypeBinding,
    ValueRef,
    descriptor_by_key,
)
from loom.target.low_descriptors import Descriptor


def _descriptor(key: str) -> Descriptor:
    return descriptor_by_key(AIE2P_CORE_DESCRIPTOR_SET, key)


def _op_emit(
    descriptor: Descriptor,
    *,
    operands: dict[str, ValueRef],
    results: dict[str, ValueRef],
    result_types: dict[str, ResultTypeBinding] | None = None,
) -> EmitDescriptorOp:
    return EmitDescriptorOp(
        descriptor=descriptor,
        operands=operands,
        results=results,
        result_types=result_types,
        form=DescriptorEmitForm.OP,
    )


class PredicateUpdate(Enum):
    """Predicate operand whose physical carrier can be updated in place."""

    NONE = "none"
    LHS = "lhs"
    RHS = "rhs"


def predicate_splat_emits(
    source: ValueRef,
    result: ValueRef,
    *,
    temporary_prefix: str = "",
    result_type: ResultTypeBinding | None = None,
) -> tuple[EmitDescriptorOp, ...]:
    """Broadcasts a scalar zero/one condition into a packed predicate mask."""

    broadcast = ValueRef.temporary(f"{temporary_prefix}broadcast_condition")
    zero = ValueRef.temporary(f"{temporary_prefix}zero")
    return (
        _op_emit(
            _descriptor("amd.xdna.aie2p.splat.i8x64"),
            operands={"src": source},
            results={"dst": broadcast},
            result_types={"dst": DescriptorResultType()},
        ),
        _op_emit(
            _descriptor("amd.xdna.aie2p.sub.i8x64"),
            operands={"s1": broadcast, "s2": broadcast},
            results={"d": zero},
            result_types={"d": DescriptorResultType()},
        ),
        _op_emit(
            _descriptor("amd.xdna.aie2p.cmp.lt.unsigned.i8x64"),
            operands={"s1": zero, "s2": broadcast},
            results={"cmp": result},
            result_types=None if result_type is None else {"cmp": result_type},
        ),
    )


def predicate_binary_emits(
    operation: str,
    lhs: ValueRef,
    rhs: ValueRef,
    result: ValueRef,
    *,
    temporary_prefix: str,
    update: PredicateUpdate,
) -> tuple[EmitDescriptorOp, ...]:
    """Applies one binary operation to both words of a predicate carrier."""

    low_result = ValueRef.temporary(f"{temporary_prefix}_low32")
    if update is PredicateUpdate.NONE:
        low = _descriptor(f"amd.xdna.aie2p.predicate.{operation}.low32")
        high = _descriptor(f"amd.xdna.aie2p.predicate.{operation}.high32")
        low_operands = {"s0": lhs, "s1": rhs}
        high_operands = {"s0": lhs, "s1": rhs, "storage": low_result}
    else:
        # The native scalar operation is commutative, so place whichever source
        # value dies here in the tied s1 encoding role.
        preserved, updated = (rhs, lhs) if update is PredicateUpdate.LHS else (lhs, rhs)
        low = _descriptor(f"amd.xdna.aie2p.predicate.{operation}.low32.rhs_tied")
        high = _descriptor(f"amd.xdna.aie2p.predicate.{operation}.high32.rhs_tied")
        low_operands = {"s0": preserved, "s1": updated}
        high_operands = {"s0": preserved, "s1": low_result}
    return (
        _op_emit(
            low,
            operands=low_operands,
            results={"d0": low_result},
            result_types={"d0": DescriptorResultType()},
        ),
        _op_emit(
            high,
            operands=high_operands,
            results={"d0": result},
            result_types={"d0": DescriptorResultType()},
        ),
    )


def predicate_select_emits(
    condition: ValueRef,
    true_value: ValueRef,
    false_value: ValueRef,
    result: ValueRef,
    *,
    temporary_prefix: str = "",
    first_update: PredicateUpdate,
) -> tuple[EmitDescriptorOp, ...]:
    """Selects packed predicate bits while keeping every value packed."""

    difference = ValueRef.temporary(f"{temporary_prefix}difference")
    changes = ValueRef.temporary(f"{temporary_prefix}changes")
    return (
        *predicate_binary_emits(
            "xor",
            false_value,
            true_value,
            difference,
            temporary_prefix=f"{temporary_prefix}difference",
            update=first_update,
        ),
        *predicate_binary_emits(
            "and",
            condition,
            difference,
            changes,
            temporary_prefix=f"{temporary_prefix}changes",
            update=PredicateUpdate.RHS,
        ),
        *predicate_binary_emits(
            "xor",
            false_value,
            changes,
            result,
            temporary_prefix=f"{temporary_prefix}selected",
            update=PredicateUpdate.RHS,
        ),
    )


def predicate_low32_select_emits(
    condition: ValueRef,
    true_value: ValueRef,
    false_value: ValueRef,
    result: ValueRef,
    *,
    temporary_prefix: str = "",
) -> tuple[EmitDescriptorOp, ...]:
    """Selects one low predicate word without consuming either input."""

    difference = ValueRef.temporary(f"{temporary_prefix}difference")
    changes = ValueRef.temporary(f"{temporary_prefix}changes")
    return (
        _op_emit(
            _descriptor("amd.xdna.aie2p.predicate.xor.low32"),
            operands={"s0": false_value, "s1": true_value},
            results={"d0": difference},
            result_types={"d0": DescriptorResultType()},
        ),
        _op_emit(
            _descriptor("amd.xdna.aie2p.predicate.and.low32.rhs_tied"),
            operands={"s0": condition, "s1": difference},
            results={"d0": changes},
            result_types={"d0": DescriptorResultType()},
        ),
        _op_emit(
            _descriptor("amd.xdna.aie2p.predicate.xor.low32.rhs_tied"),
            operands={"s0": false_value, "s1": changes},
            results={"d0": result},
            result_types={"d0": DescriptorResultType()},
        ),
    )


def x_select_mask_emit(
    condition: ValueRef,
    result: ValueRef,
) -> EmitDescriptorOp:
    """Converts a scalar zero/one condition to the inverse VSEL mask."""

    return EmitDescriptorOp(
        descriptor=_descriptor("amd.xdna.aie2p.select.mask.i32"),
        operands={"s0": condition},
        results={"d0": result},
        result_types={"d0": DescriptorResultType()},
        immediates={"imm": -1},
        form=DescriptorEmitForm.OP,
    )


def x_select_emit(
    condition_mask: ValueRef,
    true_value: ValueRef,
    false_value: ValueRef,
    result: ValueRef,
    *,
    result_type: ResultTypeBinding | None = None,
) -> EmitDescriptorOp:
    """Selects one complete 512-bit X carrier with an inverse scalar mask."""

    return _op_emit(
        _descriptor("amd.xdna.aie2p.select.i32x16"),
        operands={"s1": true_value, "s2": false_value, "sel": condition_mask},
        results={"d": result},
        result_types=None if result_type is None else {"d": result_type},
    )


def whole_x_select_emits(
    condition: ValueRef,
    true_value: ValueRef,
    false_value: ValueRef,
    result: ValueRef,
    *,
    temporary_prefix: str = "",
    result_type: ResultTypeBinding | None = None,
) -> tuple[EmitDescriptorOp, ...]:
    """Selects one X carrier from a scalar zero/one condition."""

    condition_mask = ValueRef.temporary(f"{temporary_prefix}hardware_selector")
    return (
        x_select_mask_emit(condition, condition_mask),
        x_select_emit(
            condition_mask,
            true_value,
            false_value,
            result,
            result_type=result_type,
        ),
    )
