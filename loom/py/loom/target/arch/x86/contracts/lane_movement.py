# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""x86 scalar lane movement contract rules."""

from __future__ import annotations

from dataclasses import dataclass

from loom.dialect.vector import defs as vector
from loom.target.arch.x86.contracts.rule_builders import (
    DescriptorLookup as _DescriptorLookup,
)
from loom.target.arch.x86.contracts.rule_builders import (
    emit_descriptor_op as _op_emit,
)
from loom.target.arch.x86.vector_families import (
    AVX2_CARRIER_VECTOR_BIT_WIDTHS,
    AVX512_FP16_VECTOR_BIT_WIDTHS,
    AVX512_VECTOR_BIT_WIDTHS,
    X86_LANE_FAMILIES,
)
from loom.target.contracts import (
    AttrProject,
    ContractCase,
    DescriptorResultType,
    DescriptorRule,
    EmitDescriptorOp,
    Guard,
    Scalar,
    TypePattern,
    ValueRef,
    Vector,
)
from loom.target.low_descriptors import Descriptor

_F32 = Scalar("f32")
_F64 = Scalar("f64")
_I16 = Scalar("i16")
_I64 = Scalar("i64")


@dataclass(frozen=True, slots=True)
class _LaneValueTransport:
    """Moves one logical scalar between lane and source carriers."""

    lane_type: TypePattern
    extract: Descriptor
    insert: Descriptor


def _lane_descriptor_key(mnemonic: str, element_bit_width: int) -> str:
    if mnemonic.startswith("vpextr"):
        return f"x86.avx2.{mnemonic}.gpr{max(32, element_bit_width)}.xmm"
    return f"x86.avx2.{mnemonic}.xmm"


def _chunk_descriptor_keys(vector_bit_width: int) -> tuple[str, str]:
    return {
        256: (
            "x86.avx2.vextractf128.xmm.ymm",
            "x86.avx2.vinsertf128.ymm.xmm",
        ),
        512: (
            "x86.avx512.vextractf32x4.xmm.zmm",
            "x86.avx512.vinsertf32x4.zmm.xmm",
        ),
    }[vector_bit_width]


def _lane_extract_rule(
    *,
    element_names: tuple[str, ...],
    element_bit_width: int,
    vector_bit_width: int,
    descriptor_key: str,
    descriptor_lookup: _DescriptorLookup,
    transport: _LaneValueTransport | None = None,
    priority: int = 0,
) -> DescriptorRule:
    lane_count = vector_bit_width // element_bit_width
    chunk_lane_count = 128 // element_bit_width
    source_type = Vector(element_names, lanes=lane_count)
    result_type = Scalar(element_names)
    descriptor = descriptor_lookup(descriptor_key)
    emits: list[EmitDescriptorOp] = []
    dependencies: list[Descriptor] = []
    source = ValueRef.operand("source")
    lane: AttrProject = AttrProject.i64_array_element("static_indices", element=0)
    if vector_bit_width > 128:
        extract_chunk = descriptor_lookup(_chunk_descriptor_keys(vector_bit_width)[0])
        dependencies.append(extract_chunk)
        source = ValueRef.temporary("chunk")
        lane = AttrProject.i64_array_element_remainder(
            "static_indices", element=0, divisor=chunk_lane_count
        )
        emits.append(
            _op_emit(
                descriptor=extract_chunk,
                operands={"source": ValueRef.operand("source")},
                results={"dst": source},
                result_types={"dst": DescriptorResultType()},
                immediates={
                    "lane": AttrProject.i64_array_element_quotient(
                        "static_indices", element=0, divisor=chunk_lane_count
                    )
                },
            )
        )
    lane_result = (
        ValueRef.result("result")
        if transport is None
        else ValueRef.temporary("lane_value")
    )
    emits.append(
        _op_emit(
            descriptor=descriptor,
            operands={"source": source},
            results={"dst": lane_result},
            result_types=(None if transport is None else {"dst": transport.lane_type}),
            immediates={"lane": lane},
        )
    )
    if transport is not None:
        dependencies.append(transport.extract)
        emits.append(
            _op_emit(
                descriptor=transport.extract,
                operands={"input": lane_result},
                results={"dst": ValueRef.result("result")},
            )
        )
    return DescriptorRule(
        source_op=vector.vector_extract,
        descriptor=descriptor,
        guards=(
            Guard.value_type("source", source_type),
            Guard.value_type("result", result_type),
            Guard.operand_segment_count("indices", 0),
            Guard.i64_array_count("static_indices", 1),
            Guard.i64_array_element_range("static_indices", 0, 0, lane_count - 1),
            *(Guard.descriptor_available(value) for value in dependencies),
        ),
        emit=tuple(emits),
        priority=priority,
    )


def _lane_insert_rule(
    *,
    element_names: tuple[str, ...],
    element_bit_width: int,
    vector_bit_width: int,
    descriptor_key: str,
    descriptor_lookup: _DescriptorLookup,
    transport: _LaneValueTransport | None = None,
    priority: int = 0,
) -> DescriptorRule:
    lane_count = vector_bit_width // element_bit_width
    chunk_lane_count = 128 // element_bit_width
    value_type = Scalar(element_names)
    vector_type = Vector(element_names, lanes=lane_count)
    descriptor = descriptor_lookup(descriptor_key)
    emits: list[EmitDescriptorOp] = []
    dependencies: list[Descriptor] = []
    lane_value = ValueRef.operand("value")
    if transport is not None:
        dependencies.append(transport.insert)
        lane_value = ValueRef.temporary("lane_value")
        emits.append(
            _op_emit(
                descriptor=transport.insert,
                operands={"input": ValueRef.operand("value")},
                results={"dst": lane_value},
                result_types={"dst": transport.lane_type},
            )
        )
    dest = ValueRef.operand("dest")
    lane: AttrProject = AttrProject.i64_array_element("static_indices", element=0)
    if vector_bit_width > 128:
        extract_chunk = descriptor_lookup(_chunk_descriptor_keys(vector_bit_width)[0])
        dependencies.append(extract_chunk)
        dest = ValueRef.temporary("chunk")
        lane = AttrProject.i64_array_element_remainder(
            "static_indices", element=0, divisor=chunk_lane_count
        )
        emits.append(
            _op_emit(
                descriptor=extract_chunk,
                operands={"source": ValueRef.operand("dest")},
                results={"dst": dest},
                result_types={"dst": DescriptorResultType()},
                immediates={
                    "lane": AttrProject.i64_array_element_quotient(
                        "static_indices", element=0, divisor=chunk_lane_count
                    )
                },
            )
        )
    inserted = (
        ValueRef.result("result")
        if vector_bit_width <= 128
        else ValueRef.temporary("inserted_chunk")
    )
    emits.append(
        _op_emit(
            descriptor=descriptor,
            operands={"dest": dest, "value": lane_value},
            results={"dst": inserted},
            result_types=(
                None if vector_bit_width <= 128 else {"dst": DescriptorResultType()}
            ),
            immediates={"lane": lane},
        )
    )
    primary_descriptor = descriptor
    if vector_bit_width > 128:
        insert_chunk = descriptor_lookup(_chunk_descriptor_keys(vector_bit_width)[1])
        dependencies.append(insert_chunk)
        primary_descriptor = insert_chunk
        emits.append(
            _op_emit(
                descriptor=insert_chunk,
                operands={
                    "dest": ValueRef.operand("dest"),
                    "value": inserted,
                },
                results={"dst": ValueRef.result("result")},
                immediates={
                    "lane": AttrProject.i64_array_element_quotient(
                        "static_indices", element=0, divisor=chunk_lane_count
                    )
                },
            )
        )
    return DescriptorRule(
        source_op=vector.vector_insert,
        descriptor=primary_descriptor,
        guards=(
            Guard.value_type("value", value_type),
            Guard.value_type("dest", vector_type),
            Guard.value_type("result", vector_type),
            Guard.operand_segment_count("indices", 0),
            Guard.i64_array_count("static_indices", 1),
            Guard.i64_array_element_range("static_indices", 0, 0, lane_count - 1),
            *(Guard.descriptor_available(value) for value in dependencies),
        ),
        emit=tuple(emits),
        priority=priority,
    )


def _lane_movement_rules(
    vector_bit_widths: tuple[int, ...], descriptor_lookup: _DescriptorLookup
) -> tuple[DescriptorRule, ...]:
    rules: list[DescriptorRule] = []
    for row in X86_LANE_FAMILIES:
        extract_key = _lane_descriptor_key(row.extract_mnemonic, row.element_bit_width)
        insert_key = _lane_descriptor_key(row.insert_mnemonic, row.element_bit_width)
        for vector_bit_width in vector_bit_widths:
            rules.extend(
                (
                    _lane_extract_rule(
                        element_names=row.element_names,
                        element_bit_width=row.element_bit_width,
                        vector_bit_width=vector_bit_width,
                        descriptor_key=extract_key,
                        descriptor_lookup=descriptor_lookup,
                    ),
                    _lane_insert_rule(
                        element_names=row.element_names,
                        element_bit_width=row.element_bit_width,
                        vector_bit_width=vector_bit_width,
                        descriptor_key=insert_key,
                        descriptor_lookup=descriptor_lookup,
                    ),
                )
            )
    return tuple(rules)


def _insert_f64_rule(
    lane: int,
    descriptor_lookup: _DescriptorLookup,
    *,
    vector_lane_count: int = 2,
) -> DescriptorRule:
    descriptor = descriptor_lookup("x86.avx2.vshufpd.xmm")
    # Each output lane comes from a different input. The scalar occupies the
    # low lane, so insertion selects its input order and the retained dest lane.
    return DescriptorRule(
        source_op=vector.vector_insert,
        descriptor=descriptor,
        guards=(
            Guard.value_type("value", _F64),
            Guard.value_type("dest", Vector("f64", lanes=vector_lane_count)),
            Guard.value_type("result", Vector("f64", lanes=vector_lane_count)),
            Guard.operand_segment_count("indices", 0),
            Guard.i64_array_count("static_indices", 1),
            Guard.i64_array_element_range("static_indices", 0, lane, lane),
        ),
        emit=(
            _op_emit(
                descriptor=descriptor,
                operands={
                    "lhs": ValueRef.operand("value" if lane == 0 else "dest"),
                    "rhs": ValueRef.operand("dest" if lane == 0 else "value"),
                },
                results={"dst": ValueRef.result("result")},
                immediates={"control": 2 if lane == 0 else 0},
            ),
        ),
    )


def _floating_extract_rule(
    element_name: str,
    vector_bit_width: int,
    descriptor_lookup: _DescriptorLookup,
) -> DescriptorRule:
    element_bit_width = 32 if element_name == "f32" else 64
    lane_count = vector_bit_width // element_bit_width
    chunk_lane_count = 128 // element_bit_width
    source_type = Vector(element_name, lanes=lane_count)
    result_type = Scalar(element_name)
    permute = descriptor_lookup(
        "x86.avx2.vpermilps.xmm" if element_name == "f32" else "x86.avx2.vpermilpd.xmm"
    )
    emits: list[EmitDescriptorOp] = []
    dependencies: list[Descriptor] = []
    source = ValueRef.operand("source")
    lane: AttrProject = AttrProject.i64_array_element("static_indices", element=0)
    if vector_bit_width > 128:
        extract_chunk = descriptor_lookup(_chunk_descriptor_keys(vector_bit_width)[0])
        dependencies.append(extract_chunk)
        source = ValueRef.temporary("chunk")
        lane = AttrProject.i64_array_element_remainder(
            "static_indices", element=0, divisor=chunk_lane_count
        )
        emits.append(
            _op_emit(
                descriptor=extract_chunk,
                operands={"source": ValueRef.operand("source")},
                results={"dst": source},
                result_types={"dst": DescriptorResultType()},
                immediates={
                    "lane": AttrProject.i64_array_element_quotient(
                        "static_indices", element=0, divisor=chunk_lane_count
                    )
                },
            )
        )
    emits.append(
        _op_emit(
            descriptor=permute,
            operands={"source": source},
            results={"dst": ValueRef.result("result")},
            immediates={"control": lane},
        )
    )
    return DescriptorRule(
        source_op=vector.vector_extract,
        descriptor=permute,
        guards=(
            Guard.value_type("source", source_type),
            Guard.value_type("result", result_type),
            Guard.operand_segment_count("indices", 0),
            Guard.i64_array_count("static_indices", 1),
            Guard.i64_array_element_range("static_indices", 0, 0, lane_count - 1),
            *(Guard.descriptor_available(value) for value in dependencies),
        ),
        emit=tuple(emits),
    )


def _insert_f32_rule(
    vector_bit_width: int,
    descriptor_lookup: _DescriptorLookup,
) -> DescriptorRule:
    lane_count = vector_bit_width // 32
    insert = descriptor_lookup("x86.avx2.vinsertps.xmm")
    emits: list[EmitDescriptorOp] = []
    dependencies: list[Descriptor] = []
    dest = ValueRef.operand("dest")
    lane: AttrProject = AttrProject.i64_array_element(
        "static_indices", element=0, target_bit_offset=4
    )
    if vector_bit_width > 128:
        extract_chunk = descriptor_lookup(_chunk_descriptor_keys(vector_bit_width)[0])
        dependencies.append(extract_chunk)
        dest = ValueRef.temporary("chunk")
        lane = AttrProject.i64_array_element_remainder(
            "static_indices",
            element=0,
            divisor=4,
            target_bit_offset=4,
        )
        emits.append(
            _op_emit(
                descriptor=extract_chunk,
                operands={"source": ValueRef.operand("dest")},
                results={"dst": dest},
                result_types={"dst": DescriptorResultType()},
                immediates={
                    "lane": AttrProject.i64_array_element_quotient(
                        "static_indices", element=0, divisor=4
                    )
                },
            )
        )
    inserted = (
        ValueRef.result("result")
        if vector_bit_width == 128
        else ValueRef.temporary("inserted_chunk")
    )
    emits.append(
        _op_emit(
            descriptor=insert,
            operands={"dest": dest, "value": ValueRef.operand("value")},
            results={"dst": inserted},
            result_types=(
                None if vector_bit_width == 128 else {"dst": DescriptorResultType()}
            ),
            immediates={"control": lane},
        )
    )
    primary_descriptor = insert
    if vector_bit_width > 128:
        insert_chunk = descriptor_lookup(_chunk_descriptor_keys(vector_bit_width)[1])
        dependencies.append(insert_chunk)
        primary_descriptor = insert_chunk
        emits.append(
            _op_emit(
                descriptor=insert_chunk,
                operands={
                    "dest": ValueRef.operand("dest"),
                    "value": inserted,
                },
                results={"dst": ValueRef.result("result")},
                immediates={
                    "lane": AttrProject.i64_array_element_quotient(
                        "static_indices", element=0, divisor=4
                    )
                },
            )
        )
    vector_type = Vector("f32", lanes=lane_count)
    return DescriptorRule(
        source_op=vector.vector_insert,
        descriptor=primary_descriptor,
        guards=(
            Guard.value_type("value", _F32),
            Guard.value_type("dest", vector_type),
            Guard.value_type("result", vector_type),
            Guard.operand_segment_count("indices", 0),
            Guard.i64_array_count("static_indices", 1),
            Guard.i64_array_element_range("static_indices", 0, 0, lane_count - 1),
            *(Guard.descriptor_available(value) for value in dependencies),
        ),
        emit=tuple(emits),
    )


def _insert_f64_wide_rule(
    vector_bit_width: int, descriptor_lookup: _DescriptorLookup
) -> DescriptorRule:
    extract_chunk_key, insert_chunk_key = _chunk_descriptor_keys(vector_bit_width)
    extract_chunk = descriptor_lookup(extract_chunk_key)
    move_bits = descriptor_lookup("x86.avx2.vmovq.gpr64.xmm")
    insert_lane = descriptor_lookup("x86.avx2.vpinsrq.xmm")
    insert_chunk = descriptor_lookup(insert_chunk_key)
    lane_count = vector_bit_width // 64
    return DescriptorRule(
        source_op=vector.vector_insert,
        descriptor=insert_chunk,
        guards=(
            Guard.value_type("value", _F64),
            Guard.value_type("dest", Vector("f64", lanes=lane_count)),
            Guard.value_type("result", Vector("f64", lanes=lane_count)),
            Guard.operand_segment_count("indices", 0),
            Guard.i64_array_count("static_indices", 1),
            Guard.i64_array_element_range("static_indices", 0, 0, lane_count - 1),
            Guard.descriptor_available(extract_chunk),
            Guard.descriptor_available(move_bits),
            Guard.descriptor_available(insert_lane),
        ),
        emit=(
            _op_emit(
                descriptor=extract_chunk,
                operands={"source": ValueRef.operand("dest")},
                results={"dst": ValueRef.temporary("chunk")},
                result_types={"dst": DescriptorResultType()},
                immediates={
                    "lane": AttrProject.i64_array_element_quotient(
                        "static_indices", element=0, divisor=2
                    )
                },
            ),
            _op_emit(
                descriptor=move_bits,
                operands={"input": ValueRef.operand("value")},
                results={"dst": ValueRef.temporary("value_bits")},
                result_types={"dst": _I64},
            ),
            _op_emit(
                descriptor=insert_lane,
                operands={
                    "dest": ValueRef.temporary("chunk"),
                    "value": ValueRef.temporary("value_bits"),
                },
                results={"dst": ValueRef.temporary("inserted_chunk")},
                result_types={"dst": DescriptorResultType()},
                immediates={
                    "lane": AttrProject.i64_array_element_remainder(
                        "static_indices", element=0, divisor=2
                    )
                },
            ),
            _op_emit(
                descriptor=insert_chunk,
                operands={
                    "dest": ValueRef.operand("dest"),
                    "value": ValueRef.temporary("inserted_chunk"),
                },
                results={"dst": ValueRef.result("result")},
                immediates={
                    "lane": AttrProject.i64_array_element_quotient(
                        "static_indices", element=0, divisor=2
                    )
                },
            ),
        ),
    )


def avx512_fp16_lane_movement_rules(
    descriptor_lookup: _DescriptorLookup,
) -> tuple[DescriptorRule, ...]:
    """Moves f16 lanes while preserving the feature-selected XMM scalar."""

    transport = _LaneValueTransport(
        lane_type=_I16,
        extract=descriptor_lookup("x86.avx2.vmovd.xmm.gpr32"),
        insert=descriptor_lookup("x86.avx2.vmovd.gpr32.xmm"),
    )
    return tuple(
        rule
        for vector_bit_width in AVX512_FP16_VECTOR_BIT_WIDTHS
        for rule in (
            _lane_extract_rule(
                element_names=("f16",),
                element_bit_width=16,
                vector_bit_width=vector_bit_width,
                descriptor_key=_lane_descriptor_key("vpextrw", 16),
                descriptor_lookup=descriptor_lookup,
                transport=transport,
                priority=1,
            ),
            _lane_insert_rule(
                element_names=("f16",),
                element_bit_width=16,
                vector_bit_width=vector_bit_width,
                descriptor_key=_lane_descriptor_key("vpinsrw", 16),
                descriptor_lookup=descriptor_lookup,
                transport=transport,
                priority=1,
            ),
        )
    )


def avx2_lane_movement_rules(
    descriptor_lookup: _DescriptorLookup,
) -> tuple[ContractCase, ...]:
    return (
        *_lane_movement_rules(AVX2_CARRIER_VECTOR_BIT_WIDTHS, descriptor_lookup),
        *(
            _floating_extract_rule(element_name, vector_bit_width, descriptor_lookup)
            for element_name in ("f32", "f64")
            for vector_bit_width in AVX2_CARRIER_VECTOR_BIT_WIDTHS
        ),
        *(
            _insert_f32_rule(vector_bit_width, descriptor_lookup)
            for vector_bit_width in AVX2_CARRIER_VECTOR_BIT_WIDTHS
        ),
        _insert_f64_rule(0, descriptor_lookup, vector_lane_count=1),
        _insert_f64_rule(0, descriptor_lookup),
        _insert_f64_rule(1, descriptor_lookup),
        _insert_f64_wide_rule(256, descriptor_lookup),
    )


def avx512_lane_movement_rules(
    descriptor_lookup: _DescriptorLookup,
) -> tuple[ContractCase, ...]:
    return (
        *_lane_movement_rules(AVX512_VECTOR_BIT_WIDTHS, descriptor_lookup),
        *(
            _floating_extract_rule(element_name, vector_bit_width, descriptor_lookup)
            for element_name in ("f32", "f64")
            for vector_bit_width in AVX512_VECTOR_BIT_WIDTHS
        ),
        *(
            _insert_f32_rule(vector_bit_width, descriptor_lookup)
            for vector_bit_width in AVX512_VECTOR_BIT_WIDTHS
        ),
        *(
            _insert_f64_wide_rule(vector_bit_width, descriptor_lookup)
            for vector_bit_width in AVX512_VECTOR_BIT_WIDTHS
        ),
    )
