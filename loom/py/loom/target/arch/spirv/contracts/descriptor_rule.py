# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Builders for rules using the SPIR-V logical core descriptor set."""

from loom.target.arch.spirv.descriptors import SPIRV_LOGICAL_CORE_DESCRIPTOR_SET
from loom.target.contracts import (
    AttrProject,
    DescriptorEmitForm,
    EmitDescriptorOp,
    Guard,
    ResultTypeBinding,
    SourceMemoryAddressMaterializer,
    SourceMemoryConstraint,
    ValueRef,
    descriptor_by_key,
)
from loom.target.emit.float_narrowing import (
    FloatNarrowingDescriptors,
    IntegerNarrowingDescriptors,
)
from loom.target.low_descriptors import Descriptor


def logical_core_descriptor(key: str) -> Descriptor:
    return descriptor_by_key(SPIRV_LOGICAL_CORE_DESCRIPTOR_SET, key)


def descriptor_feature_guards(*descriptors: Descriptor) -> tuple[Guard, ...]:
    guards: list[Guard] = []
    seen_keys: set[str] = set()
    for descriptor in descriptors:
        if not descriptor.feature_mask_words or descriptor.key in seen_keys:
            continue
        seen_keys.add(descriptor.key)
        guards.append(Guard.descriptor_available(descriptor))
    return tuple(guards)


def float_narrowing_descriptors(
    integer_suffix: str, float_suffix: str
) -> FloatNarrowingDescriptors:
    """Returns the same-width SPIR-V descriptors for exact float narrowing."""
    # Every comparison operand is nonnegative, so signed comparisons avoid
    # introducing integer-view bitcasts while preserving numeric ordering.
    integer_descriptors = IntegerNarrowingDescriptors(
        integer_bit_width=int(integer_suffix.removeprefix("i")),
        integer_constant=logical_core_descriptor(f"spirv.op_constant.{integer_suffix}"),
        integer_add=logical_core_descriptor(f"spirv.op_iadd.{integer_suffix}"),
        integer_subtract=logical_core_descriptor(f"spirv.op_isub.{integer_suffix}"),
        integer_shift_left=logical_core_descriptor(
            f"spirv.op_shift_left_logical.{integer_suffix}"
        ),
        integer_shift_right_logical=logical_core_descriptor(
            f"spirv.op_shift_right_logical.{integer_suffix}"
        ),
        integer_bitwise_and=logical_core_descriptor(
            f"spirv.op_bitwise_and.{integer_suffix}"
        ),
        integer_bitwise_or=logical_core_descriptor(
            f"spirv.op_bitwise_or.{integer_suffix}"
        ),
        integer_less_than_nonnegative=logical_core_descriptor(
            f"spirv.op_s_less_than.{integer_suffix}"
        ),
        integer_greater_than_equal_nonnegative=logical_core_descriptor(
            f"spirv.op_s_greater_than_equal.{integer_suffix}"
        ),
        integer_greater_than_nonnegative=logical_core_descriptor(
            f"spirv.op_s_greater_than.{integer_suffix}"
        ),
        integer_select=logical_core_descriptor(f"spirv.op_select.{integer_suffix}"),
    )
    return FloatNarrowingDescriptors(
        integer=integer_descriptors,
        float_constant=logical_core_descriptor(f"spirv.op_constant.{float_suffix}"),
        float_add=logical_core_descriptor(f"spirv.op_fadd.{float_suffix}"),
        reinterpret_float_as_integer=logical_core_descriptor(
            f"spirv.op_bitcast.{float_suffix}.{integer_suffix}"
        ),
        reinterpret_integer_as_float=logical_core_descriptor(
            f"spirv.op_bitcast.{integer_suffix}.{float_suffix}"
        ),
    )


def emit_descriptor_op(
    *,
    descriptor: Descriptor,
    operands: dict[str, ValueRef] | None = None,
    results: dict[str, ValueRef] | None = None,
    result_types: dict[str, ResultTypeBinding] | None = None,
    immediates: dict[str, AttrProject | int] | None = None,
    source_memory: SourceMemoryConstraint | None = None,
    source_memory_address_materializer: SourceMemoryAddressMaterializer | None = None,
) -> EmitDescriptorOp:
    return EmitDescriptorOp(
        descriptor=descriptor,
        operands={} if operands is None else operands,
        results={} if results is None else results,
        result_types=result_types,
        immediates={} if immediates is None else immediates,
        form=DescriptorEmitForm.OP,
        source_memory=source_memory,
        source_memory_address_materializer=source_memory_address_materializer,
    )
