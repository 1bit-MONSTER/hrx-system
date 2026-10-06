# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Software BF16 narrowing with nearest-even rounding and NaN preservation."""

from loom.dialect.scalar import conversion
from loom.target.arch.amdgpu.contracts.materializers import F32_VGPR_MATERIALIZER
from loom.target.contracts import (
    DescriptorEmitForm,
    DescriptorResultType,
    DescriptorRule,
    EmitDescriptorOp,
    Guard,
    Scalar,
    ValueRef,
    descriptor_by_key,
)
from loom.target.low_descriptors import DescriptorSet


def _bfloat_narrow_rule(
    descriptors: DescriptorSet, *, preserve_nan: bool
) -> DescriptorRule:
    shift_down = descriptor_by_key(descriptors, "amdgpu.v_lshrrev_b32.src0_inline")
    and_bits = descriptor_by_key(descriptors, "amdgpu.v_and_b32.lit")
    add_literal = descriptor_by_key(descriptors, "amdgpu.v_add_u32.lit")
    add = descriptor_by_key(descriptors, "amdgpu.v_add_u32")
    compare = descriptor_by_key(descriptors, "amdgpu.v_cmp_uno_f32")
    quiet = descriptor_by_key(descriptors, "amdgpu.v_or_b32.lit")
    select = descriptor_by_key(descriptors, "amdgpu.v_cndmask_b32")
    source = ValueRef.operand("input", materializer=F32_VGPR_MATERIALIZER.name)
    return DescriptorRule(
        source_op=conversion.scalar_fptrunc,
        descriptor=shift_down,
        guards=(
            Guard.value_type("input", Scalar("f32")),
            Guard.value_type("result", Scalar("bf16")),
            Guard.descriptor_available(shift_down),
            Guard.descriptor_available(and_bits),
            Guard.descriptor_available(add_literal),
            Guard.descriptor_available(add),
            *(
                (
                    Guard.descriptor_available(compare),
                    Guard.descriptor_available(quiet),
                    Guard.descriptor_available(select),
                )
                if preserve_nan
                else (Guard.value_not_nan("input"),)
            ),
        ),
        emit=(
            EmitDescriptorOp(
                descriptor=shift_down,
                operands={"value": source},
                results={"dst": ValueRef.temporary("upper")},
                result_types={"dst": ValueRef.result("result")},
                immediates={"imm32": 16},
                form=DescriptorEmitForm.OP,
            ),
            EmitDescriptorOp(
                descriptor=and_bits,
                operands={"rhs": ValueRef.temporary("upper")},
                results={"dst": ValueRef.temporary("lsb")},
                result_types={"dst": ValueRef.result("result")},
                immediates={"imm32": 1},
                form=DescriptorEmitForm.OP,
            ),
            EmitDescriptorOp(
                descriptor=add_literal,
                operands={"rhs": ValueRef.temporary("lsb")},
                results={"dst": ValueRef.temporary("bias")},
                result_types={"dst": ValueRef.result("result")},
                immediates={"imm32": 0x7FFF},
                form=DescriptorEmitForm.OP,
            ),
            EmitDescriptorOp(
                descriptor=add,
                operands={
                    "lhs": source,
                    "rhs": ValueRef.temporary("bias"),
                },
                results={"dst": ValueRef.temporary("rounded")},
                result_types={"dst": ValueRef.result("result")},
                form=DescriptorEmitForm.OP,
            ),
            EmitDescriptorOp(
                descriptor=shift_down,
                operands={"value": ValueRef.temporary("rounded")},
                results={
                    "dst": ValueRef.temporary("rounded_upper")
                    if preserve_nan
                    else ValueRef.result("result")
                },
                result_types={"dst": ValueRef.result("result")},
                immediates={"imm32": 16},
                form=DescriptorEmitForm.OP,
            ),
            *(
                (
                    EmitDescriptorOp(
                        descriptor=compare,
                        operands={
                            "lhs": source,
                            "rhs": source,
                        },
                        results={"mask": ValueRef.temporary("is_nan")},
                        result_types={"mask": DescriptorResultType()},
                    ),
                    EmitDescriptorOp(
                        descriptor=quiet,
                        operands={"rhs": ValueRef.temporary("upper")},
                        results={"dst": ValueRef.temporary("quiet_nan")},
                        result_types={"dst": ValueRef.result("result")},
                        immediates={"imm32": 0x40},
                    ),
                    EmitDescriptorOp(
                        descriptor=select,
                        operands={
                            "false_value": ValueRef.temporary("rounded_upper"),
                            "true_value": ValueRef.temporary("quiet_nan"),
                            "mask": ValueRef.temporary("is_nan"),
                        },
                        results={"dst": ValueRef.result("result")},
                    ),
                )
                if preserve_nan
                else ()
            ),
        ),
    )


def bfloat_narrow_rules(descriptors: DescriptorSet) -> tuple[DescriptorRule, ...]:
    """Uses retained non-NaN facts to omit the payload-preservation sequence."""
    return (
        _bfloat_narrow_rule(descriptors, preserve_nan=False),
        _bfloat_narrow_rule(descriptors, preserve_nan=True),
    )
