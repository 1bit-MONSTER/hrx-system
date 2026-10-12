# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""AIE2P dynamic vector-window contracts."""

from dataclasses import dataclass

from loom.dialect.vector import defs as vector
from loom.target.arch.amd.xdna.aie2p.contracts.accumulator_structural import (
    _ACCUMULATOR_SLICE_RESULT_SHAPES,
    _ACCUMULATOR_VECTOR_SHAPES,
)
from loom.target.arch.amd.xdna.aie2p.contracts.carrier import (
    VECTOR_CARRIER_SPECS,
)
from loom.target.arch.amd.xdna.aie2p.contracts.predicate_words import (
    _carrier_emits,
    _shift_word_emit,
    _slice_carriers,
    _source_word,
    _WordContribution,
)
from loom.target.arch.amd.xdna.aie2p.contracts.selection import (
    predicate_low32_select_emits,
    predicate_splat_emits,
    x_select_emit,
    x_select_mask_emit,
)
from loom.target.arch.amd.xdna.aie2p.core_descriptors import (
    AIE2P_CORE_DESCRIPTOR_SET,
)
from loom.target.contracts import (
    ContractEmit,
    DescriptorEmitForm,
    DescriptorResultType,
    DescriptorRule,
    EmitDescriptorOp,
    EmitRegisterConcat,
    EmitRegisterSlice,
    Guard,
    TypePattern,
    ValueRef,
    Vector,
    descriptor_by_key,
)
from loom.target.low_descriptors import Descriptor


def _descriptor(key: str) -> Descriptor:
    return descriptor_by_key(AIE2P_CORE_DESCRIPTOR_SET, key)


def _vector_interval(
    element_types: str | tuple[str, ...],
    minimum_lane_count: int,
    maximum_lane_count: int,
) -> TypePattern:
    if minimum_lane_count == maximum_lane_count:
        return Vector(element_types, lanes=minimum_lane_count)
    return Vector(
        element_types,
        minimum_lanes=minimum_lane_count,
        maximum_lanes=maximum_lane_count,
    )


@dataclass(frozen=True, slots=True)
class _XSliceShape:
    """One logical vector interval sharing an X-addressable carrier shape."""

    # Source-visible vector type interval.
    type_pattern: TypePattern
    # Number of logical lanes in one 512-bit packet.
    packet_lane_count: int
    # Number of packets that may contain logical lanes.
    logical_packet_count: int
    # Number of allocatable units in the physical carrier.
    register_unit_count: int
    # Low register class carrying the physical units.
    register_class: str

    @property
    def is_accumulator(self) -> bool:
        return self.register_class == "aie2p.mbms"


def _dynamic_slice_guards(
    source_type: TypePattern,
    result_type: TypePattern,
) -> tuple[Guard, ...]:
    return (
        Guard.operand_segment_count("offsets", 1),
        Guard.i64_array_count("static_offsets", 1),
        Guard.value_type("source", source_type),
        Guard.value_type("result", result_type),
    )


def _constant_emit(result: ValueRef, value: int) -> EmitDescriptorOp:
    return EmitDescriptorOp(
        descriptor=_descriptor("amd.xdna.aie2p.constant.i32.short"),
        results={"dst": result},
        result_types={"dst": DescriptorResultType()},
        immediates={"i": value},
        form=DescriptorEmitForm.CONST,
    )


def _binary_emit(
    descriptor_key: str,
    lhs: ValueRef,
    rhs: ValueRef,
    result: ValueRef,
) -> EmitDescriptorOp:
    return EmitDescriptorOp(
        descriptor=_descriptor(descriptor_key),
        operands={"s0": lhs, "s1": rhs},
        results={"d0": result},
        result_types={"d0": DescriptorResultType()},
        form=DescriptorEmitForm.OP,
    )


def _power_of_two_log2(value: int) -> int:
    assert value > 0 and value & (value - 1) == 0
    return value.bit_length() - 1


def _packet_selector_emits(
    offset: ValueRef,
    packet_lane_count: int,
    maximum_packet_index: int,
    *,
    temporary_prefix: str,
) -> tuple[tuple[ContractEmit, ...], tuple[ValueRef, ...]]:
    """Derives the one or two scalar bits selecting up to four packets."""

    if maximum_packet_index == 0:
        return (), ()

    shift = ValueRef.temporary(f"{temporary_prefix}packet_shift")
    packet_index = ValueRef.temporary(f"{temporary_prefix}packet_index")
    emits: list[ContractEmit] = [
        _constant_emit(shift, -_power_of_two_log2(packet_lane_count)),
        _binary_emit(
            "amd.xdna.aie2p.lshl.i32",
            offset,
            shift,
            packet_index,
        ),
    ]
    if maximum_packet_index == 1:
        return tuple(emits), (packet_index,)

    bit_mask = ValueRef.temporary(f"{temporary_prefix}packet_bit_0_mask")
    bit_zero = ValueRef.temporary(f"{temporary_prefix}packet_bit_0")
    bit_shift = ValueRef.temporary(f"{temporary_prefix}packet_bit_1_shift")
    bit_one = ValueRef.temporary(f"{temporary_prefix}packet_bit_1")
    emits.extend(
        (
            _constant_emit(bit_mask, 1),
            _binary_emit(
                "amd.xdna.aie2p.and.i32",
                packet_index,
                bit_mask,
                bit_zero,
            ),
            _constant_emit(bit_shift, -1),
            _binary_emit(
                "amd.xdna.aie2p.lshl.i32",
                packet_index,
                bit_shift,
                bit_one,
            ),
        )
    )
    return tuple(emits), (bit_zero, bit_one)


def _packet_local_byte_offset_emits(
    offset: ValueRef,
    packet_lane_count: int,
    element_byte_count: int,
    maximum_packet_index: int,
) -> tuple[tuple[ContractEmit, ...], ValueRef]:
    """Converts a logical lane offset to the packet-local byte offset."""

    emits: list[ContractEmit] = []
    local_offset = offset
    if maximum_packet_index > 0:
        lane_mask = ValueRef.temporary("packet_lane_mask")
        local_offset = ValueRef.temporary("packet_lane_offset")
        emits.extend(
            (
                _constant_emit(lane_mask, packet_lane_count - 1),
                _binary_emit(
                    "amd.xdna.aie2p.and.i32",
                    offset,
                    lane_mask,
                    local_offset,
                ),
            )
        )
    if element_byte_count == 1:
        return tuple(emits), local_offset

    byte_shift = ValueRef.temporary("element_byte_shift")
    byte_offset = ValueRef.temporary("packet_byte_offset")
    emits.extend(
        (
            _constant_emit(byte_shift, _power_of_two_log2(element_byte_count)),
            _binary_emit(
                "amd.xdna.aie2p.lshl.i32",
                local_offset,
                byte_shift,
                byte_offset,
            ),
        )
    )
    return tuple(emits), byte_offset


def _x_source_packets(
    shape: _XSliceShape,
    emits: list[ContractEmit],
) -> tuple[ValueRef, ...]:
    source = ValueRef.operand("source")
    if not shape.is_accumulator:
        if shape.logical_packet_count == 1:
            return (source,)
        packets = []
        for packet_index in range(shape.logical_packet_count):
            packet = ValueRef.temporary(f"source_packet_{packet_index}")
            emits.append(
                EmitRegisterSlice(
                    source=source,
                    result=packet,
                    unit_offset=2 * packet_index,
                    unit_count=2,
                )
            )
            packets.append(packet)
        return tuple(packets)

    packets = []
    move = _descriptor("amd.xdna.aie2p.move.accumulator512.to.vector512")
    for packet_index in range(shape.logical_packet_count):
        accumulator = ValueRef.temporary(f"source_accumulator_{packet_index}")
        packet = ValueRef.temporary(f"source_packet_{packet_index}")
        emits.extend(
            (
                EmitRegisterSlice(
                    source=source,
                    result=accumulator,
                    unit_offset=packet_index,
                    unit_count=1,
                ),
                EmitDescriptorOp(
                    descriptor=move,
                    operands={"src": accumulator},
                    results={"dst": packet},
                    result_types={"dst": DescriptorResultType()},
                    form=DescriptorEmitForm.OP,
                ),
            )
        )
        packets.append(packet)
    return tuple(packets)


def _x_select_pair(
    false_value: ValueRef,
    true_value: ValueRef,
    condition_mask: ValueRef,
    name: str,
    emits: list[ContractEmit],
) -> ValueRef:
    if false_value == true_value:
        return false_value
    result = ValueRef.temporary(name)
    emits.append(
        x_select_emit(
            condition_mask,
            true_value,
            false_value,
            result,
            result_type=DescriptorResultType(),
        )
    )
    return result


def _select_x_candidates(
    candidates: tuple[ValueRef, ...],
    condition_masks: tuple[ValueRef, ...],
    name: str,
    emits: list[ContractEmit],
) -> ValueRef:
    if len(set(candidates)) == 1:
        return candidates[0]
    if len(candidates) <= 2:
        assert len(condition_masks) == 1
        return _x_select_pair(
            candidates[0],
            candidates[1],
            condition_masks[0],
            name,
            emits,
        )

    assert len(candidates) <= 4 and len(condition_masks) == 2
    padded = (*candidates, *((candidates[-1],) * (4 - len(candidates))))
    low = _x_select_pair(
        padded[0],
        padded[1],
        condition_masks[0],
        f"{name}_low",
        emits,
    )
    high = _x_select_pair(
        padded[2],
        padded[3],
        condition_masks[0],
        f"{name}_high",
        emits,
    )
    return _x_select_pair(
        low,
        high,
        condition_masks[1],
        name,
        emits,
    )


def _materialize_x_result(
    shape: _XSliceShape,
    packets: tuple[ValueRef, ...],
    emits: list[ContractEmit],
) -> None:
    result = ValueRef.result("result")
    if not shape.is_accumulator:
        if len(packets) == 1:
            # The shift emitter writes a single ordinary result directly.
            return
        emits.append(EmitRegisterConcat(sources=packets, result=result))
        return

    move = _descriptor("amd.xdna.aie2p.move.vector512.to.accumulator512")
    accumulator_packets = []
    for packet_index, packet in enumerate(packets):
        accumulator = ValueRef.temporary(f"result_accumulator_{packet_index}")
        emits.append(
            EmitDescriptorOp(
                descriptor=move,
                operands={"src": packet},
                results={"dst": accumulator},
                result_types={"dst": DescriptorResultType()},
                form=DescriptorEmitForm.OP,
            )
        )
        accumulator_packets.append(accumulator)
    accumulator_packets.extend(
        (accumulator_packets[-1],)
        * (shape.register_unit_count - len(accumulator_packets))
    )
    emits.append(EmitRegisterConcat(sources=tuple(accumulator_packets), result=result))


def _dynamic_x_slice_rule(
    source_shape: _XSliceShape,
    result_shape: _XSliceShape,
) -> DescriptorRule:
    assert source_shape.packet_lane_count == result_shape.packet_lane_count
    packet_lane_count = source_shape.packet_lane_count
    element_byte_count = 64 // packet_lane_count
    maximum_packet_index = (
        source_shape.logical_packet_count - result_shape.logical_packet_count
    )
    assert maximum_packet_index >= 0

    emits: list[ContractEmit] = []
    source_packets = _x_source_packets(source_shape, emits)
    offset = ValueRef.operand("offsets")
    selector_emits, selectors = _packet_selector_emits(
        offset,
        packet_lane_count,
        maximum_packet_index,
        temporary_prefix="",
    )
    emits.extend(selector_emits)
    condition_masks = []
    for bit_index, selector in enumerate(selectors):
        condition_mask = ValueRef.temporary(f"packet_bit_{bit_index}_x_mask")
        emits.append(x_select_mask_emit(selector, condition_mask))
        condition_masks.append(condition_mask)

    selected_packets = []
    for result_packet_index in range(result_shape.logical_packet_count + 1):
        candidates = tuple(
            source_packets[
                min(packet_index + result_packet_index, len(source_packets) - 1)
            ]
            for packet_index in range(maximum_packet_index + 1)
        )
        selected_packets.append(
            _select_x_candidates(
                candidates,
                tuple(condition_masks),
                f"window_packet_{result_packet_index}",
                emits,
            )
        )

    byte_offset_emits, byte_offset = _packet_local_byte_offset_emits(
        offset,
        packet_lane_count,
        element_byte_count,
        maximum_packet_index,
    )
    emits.extend(byte_offset_emits)
    shift = _descriptor("amd.xdna.aie2p.shift.bytes.x.configured")
    result_packets = []
    for packet_index in range(result_shape.logical_packet_count):
        direct_result = (
            not result_shape.is_accumulator and result_shape.logical_packet_count == 1
        )
        result_packet = (
            ValueRef.result("result")
            if direct_result
            else ValueRef.temporary(f"result_packet_{packet_index}")
        )
        emits.append(
            EmitDescriptorOp(
                descriptor=shift,
                operands={
                    "s1": selected_packets[packet_index],
                    "s2": selected_packets[packet_index + 1],
                    "shift": byte_offset,
                },
                results={"d": result_packet},
                result_types=(None if direct_result else {"d": DescriptorResultType()}),
                form=DescriptorEmitForm.OP,
            )
        )
        result_packets.append(result_packet)
    _materialize_x_result(result_shape, tuple(result_packets), emits)
    return DescriptorRule(
        source_op=vector.vector_slice,
        descriptor=shift,
        guards=_dynamic_slice_guards(
            source_shape.type_pattern,
            result_shape.type_pattern,
        ),
        emit=tuple(emits),
    )


def _ordinary_dynamic_x_slice_rules() -> tuple[DescriptorRule, ...]:
    rules = []
    for element_types, element_byte_count, maximum_lane_count in VECTOR_CARRIER_SPECS:
        packet_lane_count = 64 // element_byte_count
        narrow_shape = _XSliceShape(
            _vector_interval(
                element_types,
                1,
                min(packet_lane_count, maximum_lane_count),
            ),
            packet_lane_count,
            1,
            2,
            "aie2p.vec256",
        )
        shapes = [narrow_shape]
        if maximum_lane_count > packet_lane_count:
            shapes.append(
                _XSliceShape(
                    _vector_interval(
                        element_types,
                        packet_lane_count + 1,
                        maximum_lane_count,
                    ),
                    packet_lane_count,
                    2,
                    4,
                    "aie2p.vec256",
                )
            )
        rules.extend(
            _dynamic_x_slice_rule(source_shape, result_shape)
            for source_shape in shapes
            for result_shape in shapes
            if result_shape.logical_packet_count <= source_shape.logical_packet_count
        )
    return tuple(rules)


def _accumulator_dynamic_x_slice_rules() -> tuple[DescriptorRule, ...]:
    rules = []
    for source in _ACCUMULATOR_VECTOR_SHAPES:
        source_shape = _XSliceShape(
            source.source_type,
            source.packet_lane_count,
            source.logical_packet_count,
            source.register_unit_count,
            "aie2p.mbms",
        )
        for result in _ACCUMULATOR_SLICE_RESULT_SHAPES:
            if (
                result.element_type != source.element_type
                or result.logical_packet_count > source.logical_packet_count
            ):
                continue
            result_shape = _XSliceShape(
                result.result_type,
                result.packet_lane_count,
                result.logical_packet_count,
                result.register_unit_count,
                result.register_class,
            )
            rules.append(_dynamic_x_slice_rule(source_shape, result_shape))
    return tuple(rules)


def _predicate_select_pair(
    false_value: ValueRef,
    true_value: ValueRef,
    condition: ValueRef,
    name: str,
    emits: list[ContractEmit],
) -> ValueRef:
    if false_value == true_value:
        return false_value
    result = ValueRef.temporary(name)
    emits.extend(
        predicate_low32_select_emits(
            condition,
            true_value,
            false_value,
            result,
            temporary_prefix=f"{name}_",
        )
    )
    return result


def _select_predicate_candidates(
    candidates: tuple[ValueRef, ...],
    conditions: tuple[ValueRef, ...],
    name: str,
    emits: list[ContractEmit],
) -> ValueRef:
    if len(set(candidates)) == 1:
        return candidates[0]
    if len(candidates) <= 2:
        assert len(conditions) == 1
        return _predicate_select_pair(
            candidates[0],
            candidates[1],
            conditions[0],
            name,
            emits,
        )

    assert len(candidates) <= 4 and len(conditions) == 2
    padded = (*candidates, *((candidates[-1],) * (4 - len(candidates))))
    low = _predicate_select_pair(
        padded[0], padded[1], conditions[0], f"{name}_low", emits
    )
    high = _predicate_select_pair(
        padded[2], padded[3], conditions[0], f"{name}_high", emits
    )
    return _predicate_select_pair(low, high, conditions[1], name, emits)


def _predicate_or_low32_emit(
    lhs: ValueRef,
    rhs: ValueRef,
    result: ValueRef,
) -> EmitDescriptorOp:
    return EmitDescriptorOp(
        descriptor=_descriptor("amd.xdna.aie2p.predicate.or.low32.rhs_tied"),
        operands={"s0": lhs, "s1": rhs},
        results={"d0": result},
        result_types={"d0": DescriptorResultType()},
        form=DescriptorEmitForm.OP,
    )


def _predicate_and_low32_emit(
    lhs: ValueRef,
    rhs: ValueRef,
    result: ValueRef,
) -> EmitDescriptorOp:
    return EmitDescriptorOp(
        descriptor=_descriptor("amd.xdna.aie2p.predicate.and.low32.rhs_tied"),
        operands={"s0": lhs, "s1": rhs},
        results={"d0": result},
        result_types={"d0": DescriptorResultType()},
        form=DescriptorEmitForm.OP,
    )


def _dynamic_predicate_slice_rule(
    source_type: TypePattern,
    source_word_count: int,
    result_type: TypePattern,
    result_word_count: int,
) -> DescriptorRule:
    emits: list[ContractEmit] = []
    source_slices, source_carriers = _slice_carriers(
        ValueRef.operand("source"),
        source_type,
        temporary_prefix="source_",
    )
    emits.extend(source_slices)

    zero = ValueRef.temporary("zero")
    emits.append(_constant_emit(zero, 0))
    source_words = []
    for word_index in range(source_word_count):
        source_word = _source_word(source_carriers, word_index)
        if source_word.part == "low32":
            source_words.append(source_word.source)
            continue
        normalized = ValueRef.temporary(f"source_word_{word_index}")
        emits.append(
            _shift_word_emit(
                _WordContribution(source_word.source, source_word.part, zero),
                destination_part="low32",
                result=normalized,
                result_is_temporary=True,
            )
        )
        source_words.append(normalized)

    maximum_word_index = source_word_count - result_word_count
    offset = ValueRef.operand("offsets")
    selector_emits, selectors = _packet_selector_emits(
        offset,
        32,
        maximum_word_index,
        temporary_prefix="word_",
    )
    emits.extend(selector_emits)
    conditions = []
    for bit_index, selector in enumerate(selectors):
        condition = ValueRef.temporary(f"word_bit_{bit_index}_condition")
        emits.extend(
            predicate_splat_emits(
                selector,
                condition,
                temporary_prefix=f"word_bit_{bit_index}_",
                result_type=DescriptorResultType(),
            )
        )
        conditions.append(condition)

    selected_words = []
    for result_word_index in range(result_word_count + 1):
        candidates = tuple(
            source_words[min(word_index + result_word_index, len(source_words) - 1)]
            for word_index in range(maximum_word_index + 1)
        )
        selected_words.append(
            _select_predicate_candidates(
                candidates,
                tuple(conditions),
                f"window_word_{result_word_index}",
                emits,
            )
        )

    within = offset
    if maximum_word_index > 0:
        within_mask = ValueRef.temporary("word_lane_mask")
        within = ValueRef.temporary("word_lane_offset")
        emits.extend(
            (
                _constant_emit(within_mask, 31),
                _binary_emit(
                    "amd.xdna.aie2p.and.i32",
                    offset,
                    within_mask,
                    within,
                ),
            )
        )
    right_shift = ValueRef.temporary("word_right_shift")
    emits.append(
        _binary_emit(
            "amd.xdna.aie2p.sub.i32",
            zero,
            within,
            right_shift,
        )
    )

    crossing_words = tuple(
        word_index
        for word_index in range(result_word_count)
        if selected_words[word_index] != selected_words[word_index + 1]
    )
    left_shift = ValueRef.temporary("word_left_shift")
    nonzero_condition = ValueRef.temporary("word_nonzero_condition")
    if crossing_words:
        word_bits = ValueRef.temporary("word_bits")
        emits.extend(
            (
                _constant_emit(word_bits, 32),
                _binary_emit(
                    "amd.xdna.aie2p.sub.i32",
                    word_bits,
                    within,
                    left_shift,
                ),
            )
        )
        emits.extend(
            predicate_splat_emits(
                within,
                nonzero_condition,
                temporary_prefix="word_nonzero_",
                result_type=DescriptorResultType(),
            )
        )

    result_words = []
    for word_index in range(result_word_count):
        right = ValueRef.temporary(f"result_word_{word_index}_right")
        emits.append(
            _shift_word_emit(
                _WordContribution(
                    selected_words[word_index],
                    "low32",
                    right_shift,
                ),
                destination_part="low32",
                result=right,
                result_is_temporary=True,
            )
        )
        if word_index not in crossing_words:
            result_words.append(right)
            continue

        left = ValueRef.temporary(f"result_word_{word_index}_left")
        masked_left = ValueRef.temporary(f"result_word_{word_index}_masked_left")
        combined = ValueRef.temporary(f"result_word_{word_index}")
        emits.extend(
            (
                _shift_word_emit(
                    _WordContribution(
                        selected_words[word_index + 1],
                        "low32",
                        left_shift,
                    ),
                    destination_part="low32",
                    result=left,
                    result_is_temporary=True,
                ),
                _predicate_and_low32_emit(nonzero_condition, left, masked_left),
                _predicate_or_low32_emit(right, masked_left, combined),
            )
        )
        result_words.append(combined)

    result_carriers = []
    for carrier_index in range((result_word_count + 1) // 2):
        result_is_temporary = result_word_count > 2
        result = (
            ValueRef.temporary(f"result_carrier_{carrier_index}")
            if result_is_temporary
            else ValueRef.result("result")
        )
        low_word_index = 2 * carrier_index
        high_word_index = low_word_index + 1
        emits.extend(
            _carrier_emits(
                (
                    _WordContribution(
                        result_words[low_word_index],
                        "low32",
                    ),
                ),
                (
                    (
                        _WordContribution(
                            result_words[high_word_index],
                            "low32",
                        ),
                    )
                    if high_word_index < result_word_count
                    else ()
                ),
                result,
                temporary_prefix=f"result_carrier_{carrier_index}_",
                result_is_temporary=result_is_temporary,
            )
        )
        result_carriers.append(result)
    if len(result_carriers) > 1:
        emits.append(
            EmitRegisterConcat(
                sources=tuple(result_carriers),
                result=ValueRef.result("result"),
            )
        )

    primary_descriptor = next(
        emit.descriptor
        for emit in reversed(emits)
        if isinstance(emit, EmitDescriptorOp)
    )
    return DescriptorRule(
        source_op=vector.vector_slice,
        descriptor=primary_descriptor,
        guards=_dynamic_slice_guards(source_type, result_type),
        emit=tuple(emits),
    )


def _dynamic_predicate_slice_rules() -> tuple[DescriptorRule, ...]:
    rules = []
    for source_type, source_word_count in (
        (Vector("i1", minimum_lanes=1, maximum_lanes=64), 2),
        (Vector("i1", minimum_lanes=65, maximum_lanes=128), 4),
    ):
        rules.extend(
            _dynamic_predicate_slice_rule(
                source_type,
                source_word_count,
                Vector(
                    "i1",
                    minimum_lanes=(result_word_count - 1) * 32 + 1,
                    maximum_lanes=result_word_count * 32,
                ),
                result_word_count,
            )
            for result_word_count in range(1, source_word_count + 1)
        )
    return tuple(rules)


AIE2P_DYNAMIC_VECTOR_SLICE_RULES = (
    *_ordinary_dynamic_x_slice_rules(),
    *_accumulator_dynamic_x_slice_rules(),
    *_dynamic_predicate_slice_rules(),
)
