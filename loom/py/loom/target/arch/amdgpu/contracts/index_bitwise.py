# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Native bit operations on AMDGPU's 32-bit index carrier."""

from collections.abc import Iterable

from loom.dialect.index import defs as index
from loom.dsl import Op
from loom.target.arch.amdgpu.contracts.materializers import (
    ADDRESS_SGPR_MATERIALIZER,
    ADDRESS_VGPR_MATERIALIZER,
)
from loom.target.contracts import (
    DescriptorRule,
    EmitDescriptorOp,
    Guard,
    Scalar,
    ValueRef,
    descriptor_by_key,
)
from loom.target.low_descriptors import DescriptorSet

_INDEX = Scalar("index")
_RESULT = ValueRef.result("result")


def _register_guards(fields: tuple[str, ...], register_class: str) -> tuple[Guard, ...]:
    materializer = (
        ADDRESS_SGPR_MATERIALIZER
        if register_class == "amdgpu.sgpr"
        else ADDRESS_VGPR_MATERIALIZER
    )
    return (
        *(Guard.value_type(field, _INDEX) for field in (*fields, "result")),
        Guard.low_value_register_class("result", register_class),
        Guard.low_value_register_unit_count("result", 1),
        *(Guard.value_materializable(field, materializer.name) for field in fields),
    )


def _operand(field: str, register_class: str) -> ValueRef:
    materializer = (
        ADDRESS_SGPR_MATERIALIZER
        if register_class == "amdgpu.sgpr"
        else ADDRESS_VGPR_MATERIALIZER
    )
    return ValueRef.operand(field, materializer=materializer.name)


def _rule(
    source_op: Op,
    fields: tuple[str, ...],
    register_class: str,
    emits: tuple[EmitDescriptorOp, ...],
) -> DescriptorRule:
    descriptors = {emit.descriptor.key: emit.descriptor for emit in emits}
    return DescriptorRule(
        source_op=source_op,
        descriptor=emits[-1].descriptor,
        guards=(
            *_register_guards(fields, register_class),
            *(
                Guard.descriptor_available(descriptor)
                for descriptor in descriptors.values()
            ),
        ),
        emit=emits,
    )


def index_bitwise_rules(descriptor_set: DescriptorSet) -> Iterable[DescriptorRule]:
    for register_class, prefix in (("amdgpu.sgpr", "s"), ("amdgpu.vgpr", "v")):
        move = descriptor_by_key(descriptor_set, f"amdgpu.{prefix}_mov_b32")
        minimum = descriptor_by_key(descriptor_set, f"amdgpu.{prefix}_min_u32")
        for source_op, descriptor_key in (
            (index.index_ctlzi, f"amdgpu.{prefix}_clz_i32_u32"),
            (index.index_cttzi, f"amdgpu.{prefix}_ctz_i32_b32"),
            (
                index.index_ctpopi,
                "amdgpu.s_bcnt1_i32_b32"
                if prefix == "s"
                else "amdgpu.v_bcnt_u32_b32.src1_zero",
            ),
        ):
            native_count = descriptor_by_key(descriptor_set, descriptor_key)
            # CLZ/CTZ return UINT32_MAX for zero. Unsigned min supplies the
            # carrier width while leaving every nonzero count unchanged.
            repairs_zero = source_op is not index.index_ctpopi
            emits = (
                EmitDescriptorOp(
                    descriptor=native_count,
                    operands={"input": _operand("input", register_class)},
                    results={
                        "dst": ValueRef.temporary("count") if repairs_zero else _RESULT
                    },
                    result_types={"dst": _RESULT},
                ),
            )
            if repairs_zero:
                emits += (
                    EmitDescriptorOp(
                        descriptor=move,
                        results={"dst": ValueRef.temporary("width")},
                        result_types={"dst": _RESULT},
                        immediates={"imm32": 32},
                    ),
                    EmitDescriptorOp(
                        descriptor=minimum,
                        operands={
                            "lhs": ValueRef.temporary("count"),
                            "rhs": ValueRef.temporary("width"),
                        },
                        results={"dst": _RESULT},
                    ),
                )
            yield _rule(source_op, ("input",), register_class, emits)

    scalar_subtract = descriptor_by_key(descriptor_set, "amdgpu.s_sub_u32")
    scalar_move = descriptor_by_key(descriptor_set, "amdgpu.s_mov_b32")
    scalar_or = descriptor_by_key(descriptor_set, "amdgpu.s_or_b32")
    vector_subtract = descriptor_by_key(descriptor_set, "amdgpu.v_sub_u32")
    vector_move = descriptor_by_key(descriptor_set, "amdgpu.v_mov_b32")
    align = descriptor_by_key(descriptor_set, "amdgpu.v_alignbit_b32")
    for source_op, first_shift, second_shift in (
        (index.index_rotli, "s_lshl_b32", "s_lshr_b32"),
        (index.index_rotri, "s_lshr_b32", "s_lshl_b32"),
    ):
        # Scalar shifts mask their count to five bits. At zero, both halves
        # reproduce the input, so their OR is the identity without a branch.
        yield _rule(
            source_op,
            ("lhs", "rhs"),
            "amdgpu.sgpr",
            (
                EmitDescriptorOp(
                    descriptor=scalar_move,
                    results={"dst": ValueRef.temporary("width")},
                    result_types={"dst": _RESULT},
                    immediates={"imm32": 32},
                ),
                EmitDescriptorOp(
                    descriptor=scalar_subtract,
                    operands={
                        "lhs": ValueRef.temporary("width"),
                        "rhs": _operand("rhs", "amdgpu.sgpr"),
                    },
                    results={"dst": ValueRef.temporary("complement")},
                    result_types={"dst": _RESULT},
                ),
                EmitDescriptorOp(
                    descriptor=descriptor_by_key(
                        descriptor_set, f"amdgpu.{first_shift}"
                    ),
                    operands={
                        "lhs": _operand("lhs", "amdgpu.sgpr"),
                        "rhs": _operand("rhs", "amdgpu.sgpr"),
                    },
                    results={"dst": ValueRef.temporary("first")},
                    result_types={"dst": _RESULT},
                ),
                EmitDescriptorOp(
                    descriptor=descriptor_by_key(
                        descriptor_set, f"amdgpu.{second_shift}"
                    ),
                    operands={
                        "lhs": _operand("lhs", "amdgpu.sgpr"),
                        "rhs": ValueRef.temporary("complement"),
                    },
                    results={"dst": ValueRef.temporary("second")},
                    result_types={"dst": _RESULT},
                ),
                EmitDescriptorOp(
                    descriptor=scalar_or,
                    operands={
                        "lhs": ValueRef.temporary("first"),
                        "rhs": ValueRef.temporary("second"),
                    },
                    results={"dst": _RESULT},
                ),
            ),
        )
        shift = _operand("rhs", "amdgpu.vgpr")
        rotation_prefix: tuple[EmitDescriptorOp, ...] = ()
        if source_op is index.index_rotli:
            rotation_prefix = (
                EmitDescriptorOp(
                    descriptor=vector_move,
                    results={"dst": ValueRef.temporary("zero")},
                    result_types={"dst": _RESULT},
                    immediates={"imm32": 0},
                ),
                EmitDescriptorOp(
                    descriptor=vector_subtract,
                    operands={"lhs": ValueRef.temporary("zero"), "rhs": shift},
                    results={"dst": ValueRef.temporary("complement")},
                    result_types={"dst": _RESULT},
                ),
            )
            shift = ValueRef.temporary("complement")
        yield _rule(
            source_op,
            ("lhs", "rhs"),
            "amdgpu.vgpr",
            (
                *rotation_prefix,
                EmitDescriptorOp(
                    descriptor=align,
                    operands={
                        "high": _operand("lhs", "amdgpu.vgpr"),
                        "low": _operand("lhs", "amdgpu.vgpr"),
                        "shift": shift,
                    },
                    results={"dst": _RESULT},
                ),
            ),
        )
