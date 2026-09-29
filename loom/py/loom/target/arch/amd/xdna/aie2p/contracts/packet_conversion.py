# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Native AIE2P packet-conversion contracts."""

from __future__ import annotations

from dataclasses import dataclass
from itertools import product

from loom.dialect.vector import defs as vector
from loom.dsl import Op
from loom.target.arch.amd.xdna.aie2p.contracts.data_path import (
    BF16_CONVERSION_ROUNDING,
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
    SourceNode,
    ValueProject,
    ValueRef,
    Vector,
    descriptor_by_key,
)
from loom.target.low_descriptors import Descriptor

# Source-visible lane counts supported by native BF16/F32 packet conversion.
BF16_F32_PACKET_LANE_COUNTS = (16, 32)

# Packed i4 byte counts consumed by native VUNPACK forms. Each input byte
# produces two sign- or zero-extended i8 lanes.
I4_UNPACK_SOURCE_LANE_COUNTS = (32, 64)


@dataclass(frozen=True, slots=True)
class IntegerWidenInstruction:
    """One physical vector shape consumed and produced by a VUPS form."""

    # Source vector element type.
    input_element: str
    # Result vector element type.
    result_element: str
    # Native lane count converted by the instruction.
    native_lane_count: int

    @property
    def memory_width_bits(self) -> int:
        """Number of source bits consumed by a fused widening load."""

        return self.native_lane_count * int(self.input_element[1:])

    @property
    def result_width_bits(self) -> int:
        """Number of result bits produced by the widening operation."""

        return self.native_lane_count * int(self.result_element[1:])

    @property
    def accumulator_unit_count(self) -> int:
        """Number of 512-bit accumulator units produced by VUPS."""

        return self.result_width_bits // 512

    @property
    def physical_shape(self) -> str:
        """Physical VUPS width relation encoded by the instruction."""

        widening_factor = self.result_width_bits // self.memory_width_bits
        source_carrier = "w" if self.memory_width_bits == 256 else "x"
        result_carrier = {1: "b", 2: "c", 4: "d"}[self.accumulator_unit_count]
        return f"{widening_factor}x.{source_carrier}-to-{result_carrier}"

    @property
    def ups_mode(self) -> int:
        """AIE2P crUpsMode value selecting the result element width."""

        return int(self.result_element[1:]) // 32 - 1

    @property
    def slice_input(self) -> bool:
        """Whether the W source is sliced from its ordinary X carrier."""

        return self.memory_width_bits == 256

    @property
    def direct_accumulator_result(self) -> bool:
        """Whether the result remains natively in the accumulator file."""

        return self.accumulator_unit_count == 4


@dataclass(frozen=True, slots=True)
class IntegerWidenRuleShape:
    """Logical lane interval realized by one physical VUPS form."""

    # Physical instruction shape used for the conversion.
    instruction: IntegerWidenInstruction
    # First logical lane count realized by this rule.
    minimum_lane_count: int
    # Last logical lane count realized by this rule.
    maximum_lane_count: int
    # Number of 512-bit result units containing logical lanes.
    result_accumulator_unit_count: int

    def __post_init__(self) -> None:
        if not (
            1
            <= self.minimum_lane_count
            <= self.maximum_lane_count
            <= self.instruction.native_lane_count
        ):
            raise ValueError("integer widening logical lane interval is invalid")
        if not (
            1
            <= self.result_accumulator_unit_count
            <= self.instruction.accumulator_unit_count
        ):
            raise ValueError("integer widening result unit count is invalid")
        result_lanes_per_unit = 512 // int(self.instruction.result_element[1:])
        if (
            self.minimum_lane_count
            <= (self.result_accumulator_unit_count - 1) * result_lanes_per_unit
            or self.maximum_lane_count
            > self.result_accumulator_unit_count * result_lanes_per_unit
        ):
            raise ValueError(
                "integer widening logical interval crosses a result carrier boundary"
            )

    @property
    def input_type(self) -> Vector:
        """Source-visible input type interval."""

        return Vector(
            self.instruction.input_element,
            minimum_lanes=self.minimum_lane_count,
            maximum_lanes=self.maximum_lane_count,
        )

    @property
    def result_type(self) -> Vector:
        """Source-visible result type interval."""

        return Vector(
            self.instruction.result_element,
            minimum_lanes=self.minimum_lane_count,
            maximum_lanes=self.maximum_lane_count,
        )

    def report_key(self, signedness: str) -> str:
        """Stable compile-report key for this logical interval."""

        lane_range = (
            str(self.minimum_lane_count)
            if self.minimum_lane_count == self.maximum_lane_count
            else f"{self.minimum_lane_count}-{self.maximum_lane_count}"
        )
        return (
            f"native_{signedness}_{self.instruction.input_element}x{lane_range}_to_"
            f"{self.instruction.result_element}x{lane_range}"
        )


@dataclass(frozen=True, slots=True)
class IntegerPackCase:
    """One source-visible shape supported by native VPACK forms."""

    # Source vector element type.
    input_element: str
    # Number of source lanes packed by the conversion.
    input_lanes: int
    # Required vector.bitpack width, or None for vector.trunci.
    bit_width: int | None

    @property
    def source_op(self) -> Op:
        """Source conversion operation selecting the pack semantics."""

        return vector.vector_trunci if self.bit_width is None else vector.vector_bitpack

    @property
    def source_field(self) -> str:
        """Source operand field consumed by the conversion."""

        return "input" if self.bit_width is None else "source"

    @property
    def output_element_bits(self) -> int:
        """Logical result element width packed by VPACK."""

        return 8 if self.bit_width is None else self.bit_width

    @property
    def result_lanes(self) -> int:
        """Number of physical i8 lanes carrying the packed result."""

        return self.input_lanes * self.output_element_bits // 8

    @property
    def memory_width_bits(self) -> int:
        """Number of result bits written by a fused packing store."""

        return self.result_lanes * 8

    @property
    def physical_width(self) -> str:
        """Physical VPACK result carrier width."""

        return {256: "w", 512: "x"}[self.memory_width_bits]

    @property
    def pack_size(self) -> int:
        """AIE2P crPackSize value selecting the result element width."""

        return self.output_element_bits.bit_length() - 3

    @property
    def report_key(self) -> str:
        """Stable compile-report key for the standalone conversion."""

        if self.bit_width is None:
            return (
                f"native_trunc_{self.input_element}x{self.input_lanes}_to_"
                f"i8x{self.result_lanes}"
            )
        return (
            f"native_bitpack_{self.input_element}x{self.input_lanes}_to_"
            f"i{self.bit_width}x{self.input_lanes}"
        )

    @property
    def pad_result(self) -> bool:
        """Whether the result preserves an unused X-carrier half."""

        return self.memory_width_bits == 256


_I16_TO_I32_W = IntegerWidenInstruction("i16", "i32", 16)
_I32_TO_I64_W = IntegerWidenInstruction("i32", "i64", 8)
_I8_TO_I32_W = IntegerWidenInstruction("i8", "i32", 32)
_I16_TO_I64_W = IntegerWidenInstruction("i16", "i64", 16)
_I16_TO_I32_X = IntegerWidenInstruction("i16", "i32", 32)
_I32_TO_I64_X = IntegerWidenInstruction("i32", "i64", 16)
_I8_TO_I32_X = IntegerWidenInstruction("i8", "i32", 64)
_I16_TO_I64_X = IntegerWidenInstruction("i16", "i64", 32)

# Exact physical shapes also own fused memory rules, whose access width cannot
# exceed the source value's logical footprint.
INTEGER_WIDEN_INSTRUCTIONS = (
    _I16_TO_I32_W,
    _I32_TO_I64_W,
    _I8_TO_I32_W,
    _I16_TO_I64_W,
    _I16_TO_I32_X,
    _I32_TO_I64_X,
    _I8_TO_I32_X,
    _I16_TO_I64_X,
)

# Standalone conversions can consume every physical lane in the source's X
# carrier because integer VUPS is nontrapping and lanes outside the logical
# value domain remain unobservable. Keep each interval within one result
# carrier count so emission retains exactly the units containing logical lanes.
INTEGER_WIDEN_RULE_SHAPES = (
    *(
        IntegerWidenRuleShape(
            instruction,
            instruction.native_lane_count,
            instruction.native_lane_count,
            instruction.accumulator_unit_count,
        )
        for instruction in INTEGER_WIDEN_INSTRUCTIONS
    ),
    IntegerWidenRuleShape(_I16_TO_I32_W, 1, 15, 1),
    IntegerWidenRuleShape(_I32_TO_I64_W, 1, 7, 1),
    IntegerWidenRuleShape(_I8_TO_I32_W, 1, 16, 1),
    IntegerWidenRuleShape(_I8_TO_I32_W, 17, 31, 2),
    IntegerWidenRuleShape(_I16_TO_I64_W, 1, 8, 1),
    IntegerWidenRuleShape(_I16_TO_I64_W, 9, 15, 2),
    IntegerWidenRuleShape(_I16_TO_I32_X, 17, 31, 2),
    IntegerWidenRuleShape(_I32_TO_I64_X, 9, 15, 2),
)

INTEGER_PACK_CASES = (
    IntegerPackCase("i16", 32, None),
    IntegerPackCase("i16", 64, None),
    IntegerPackCase("i8", 64, 4),
    IntegerPackCase("i8", 128, 4),
)


def _descriptor(key: str) -> Descriptor:
    return descriptor_by_key(AIE2P_CORE_DESCRIPTOR_SET, key)


def _exact_vector(element: str, element_count: int) -> Vector:
    return Vector(element, lanes=element_count)


def integer_widen_state_emits(
    ups_mode: int,
) -> tuple[ValueRef, tuple[ContractEmit, ...]]:
    """Builds the explicit configured state consumed by one VUPS form."""

    shift = ValueRef.temporary("shift")
    return shift, (
        EmitDescriptorOp(
            descriptor=_descriptor("amd.xdna.aie2p.constant.i32.shift"),
            results={"dst": shift},
            result_types={"dst": DescriptorResultType()},
            immediates={"i": 0},
            form=DescriptorEmitForm.CONST,
        ),
        EmitDescriptorOp(
            descriptor=_descriptor("amd.xdna.aie2p.state.saturation.immediate"),
            immediates={"i": 0},
            form=DescriptorEmitForm.OP,
        ),
        EmitDescriptorOp(
            descriptor=_descriptor("amd.xdna.aie2p.state.ups-mode.immediate"),
            immediates={"i": ups_mode},
            form=DescriptorEmitForm.OP,
        ),
    )


def integer_widen_result_emits(
    instruction: IntegerWidenInstruction,
    result: ValueRef,
    result_accumulator_unit_count: int | None = None,
) -> tuple[ValueRef, tuple[ContractEmit, ...]]:
    """Bridges a native accumulator result to its source-visible carrier."""

    if result_accumulator_unit_count is None:
        result_accumulator_unit_count = instruction.accumulator_unit_count
    if (
        instruction.direct_accumulator_result
        and result_accumulator_unit_count == instruction.accumulator_unit_count
    ):
        return result, ()

    native_result = ValueRef.temporary("wide_result")
    output_emits: list[ContractEmit] = []
    vector_units: list[ValueRef] = []
    move_from_accumulator = _descriptor(
        "amd.xdna.aie2p.move.accumulator512.to.vector512"
    )
    for unit in range(result_accumulator_unit_count):
        accumulator_unit = native_result
        if instruction.accumulator_unit_count > 1:
            accumulator_unit = ValueRef.temporary(f"accumulator_unit_{unit}")
            output_emits.append(
                EmitRegisterSlice(
                    source=native_result,
                    result=accumulator_unit,
                    unit_offset=unit,
                    unit_count=1,
                )
            )
        vector_unit = (
            result
            if result_accumulator_unit_count == 1
            else ValueRef.temporary(f"vector_unit_{unit}")
        )
        output_emits.append(
            EmitDescriptorOp(
                descriptor=move_from_accumulator,
                operands={"src": accumulator_unit},
                results={"dst": vector_unit},
                result_types=(
                    {"dst": DescriptorResultType()}
                    if result_accumulator_unit_count > 1
                    else None
                ),
                form=DescriptorEmitForm.OP,
            )
        )
        vector_units.append(vector_unit)
    if result_accumulator_unit_count > 1:
        output_emits.append(
            EmitRegisterConcat(
                sources=vector_units,
                result=result,
            )
        )
    return native_result, tuple(output_emits)


def integer_pack_state_emits(
    pack_size: int, *, saturation: int = 0
) -> tuple[ContractEmit, ...]:
    """Builds the explicit configured state consumed by one VPACK form."""

    return (
        EmitDescriptorOp(
            descriptor=_descriptor("amd.xdna.aie2p.state.saturation.immediate"),
            immediates={"i": saturation},
            form=DescriptorEmitForm.OP,
        ),
        EmitDescriptorOp(
            descriptor=_descriptor("amd.xdna.aie2p.state.pack-size.immediate"),
            immediates={"i": pack_size},
            form=DescriptorEmitForm.OP,
        ),
    )


def integer_unpack_state_emits() -> tuple[ContractEmit, ...]:
    """Builds the configured state consumed by one four-bit VUNPACK form."""

    return (
        EmitDescriptorOp(
            descriptor=_descriptor("amd.xdna.aie2p.state.unpack-size.immediate"),
            immediates={"i": 0},
            form=DescriptorEmitForm.OP,
        ),
    )


def _integer_bitunpack_rule(
    source_op: Op,
    source_kind: str,
    source_lane_count: int,
) -> DescriptorRule:
    result_lane_count = source_lane_count * 2
    unpack = _descriptor(
        f"amd.xdna.aie2p.unpack.{source_kind}4x{result_lane_count}.to."
        f"{source_kind}8x{result_lane_count}.configured"
    )
    source = ValueRef.operand("source")
    input_emits: tuple[ContractEmit, ...] = ()
    if source_lane_count == 32:
        source = ValueRef.temporary("packed_source")
        input_emits = (
            EmitRegisterSlice(
                source=ValueRef.operand("source"),
                result=source,
                unit_count=1,
            ),
        )
    signedness = "unsigned" if source_kind == "u" else "signed"
    return DescriptorRule(
        source_op=source_op,
        descriptor=unpack,
        guards=(
            Guard.value_type("source", _exact_vector("i8", source_lane_count)),
            Guard.value_type("result", _exact_vector("i8", result_lane_count)),
            Guard.attr_kind("width", "i64"),
            Guard.i64_range("width", 4, 4),
        ),
        emit=(
            *input_emits,
            *integer_unpack_state_emits(),
            EmitDescriptorOp(
                descriptor=unpack,
                operands={"src": source},
                results={"dst": ValueRef.result("result")},
                form=DescriptorEmitForm.OP,
            ),
        ),
        report_key=(
            f"native_{signedness}_i4x{result_lane_count}_to_i8x{result_lane_count}"
        ),
    )


def _integer_widen_rule(
    source_op: Op,
    signedness: str,
    rule_shape: IntegerWidenRuleShape,
) -> DescriptorRule:
    instruction = rule_shape.instruction
    source = ValueRef.operand("input")
    input_emits: tuple[ContractEmit, ...] = ()
    if instruction.slice_input:
        source = ValueRef.temporary("source_w")
        input_emits = (
            EmitRegisterSlice(
                source=ValueRef.operand("input"),
                result=source,
                unit_count=1,
            ),
        )
    shift, state_emits = integer_widen_state_emits(instruction.ups_mode)
    result = ValueRef.result("result")
    native_result, output_emits = integer_widen_result_emits(
        instruction,
        result,
        rule_shape.result_accumulator_unit_count,
    )
    widen = _descriptor(
        f"amd.xdna.aie2p.widen.{instruction.physical_shape}.{signedness}.configured"
    )
    return DescriptorRule(
        source_op=source_op,
        descriptor=widen,
        guards=(
            Guard.value_type("input", rule_shape.input_type),
            Guard.value_type("result", rule_shape.result_type),
        ),
        emit=(
            *input_emits,
            *state_emits,
            EmitDescriptorOp(
                descriptor=widen,
                operands={"src": source, "su": shift},
                results={"dst": native_result},
                result_types=(
                    None
                    if (
                        instruction.direct_accumulator_result
                        and rule_shape.result_accumulator_unit_count
                        == instruction.accumulator_unit_count
                    )
                    else {"dst": DescriptorResultType()}
                ),
                form=DescriptorEmitForm.OP,
            ),
            *output_emits,
        ),
        report_key=rule_shape.report_key(signedness),
    )


def _integer_shift_rule(source_op: Op) -> DescriptorRule:
    """Shifts uniform i32 packets through exact accumulator widening."""

    packet = _exact_vector("i32", 16)
    signedness = "signed" if source_op is vector.vector_shrsi else "unsigned"
    widen = _descriptor(f"amd.xdna.aie2p.widen.2x.x-to-c.{signedness}.configured")
    narrow = _descriptor(f"amd.xdna.aie2p.narrow.2x.c-to-x.{signedness}.configured")
    shift_left = source_op is vector.vector_shli
    distance = ValueProject.exact_i64("rhs")
    return DescriptorRule(
        source_op=source_op,
        descriptor=widen,
        guards=(
            *(Guard.value_type(field, packet) for field in ("lhs", "rhs", "result")),
            Guard.value_exact_i64("rhs"),
            Guard.value_i64_range("rhs", 0, 31),
        ),
        emit=(
            *(
                EmitDescriptorOp(
                    descriptor=_descriptor("amd.xdna.aie2p.constant.i32.shift"),
                    results={"dst": ValueRef.temporary(name)},
                    result_types={"dst": DescriptorResultType()},
                    immediates={"i": amount},
                    form=DescriptorEmitForm.CONST,
                )
                for name, amount in (
                    ("upshift", distance if shift_left else 0),
                    ("downshift", 0 if shift_left else distance),
                )
            ),
            # Widen to i64 before shifting. Unsaturated SRS then selects the
            # low i32 bits; floor rounding preserves arithmetic right shift.
            *(
                EmitDescriptorOp(
                    descriptor=_descriptor(f"amd.xdna.aie2p.state.{name}.immediate"),
                    immediates={"i": value},
                    form=DescriptorEmitForm.OP,
                )
                for name, value in (
                    ("saturation", 0),
                    ("ups-mode", 1),
                    ("srs-mode", 1),
                    ("rounding", 0),
                )
            ),
            EmitDescriptorOp(
                descriptor=widen,
                operands={
                    "src": ValueRef.operand("lhs"),
                    "su": ValueRef.temporary("upshift"),
                },
                results={"dst": ValueRef.temporary("wide")},
                result_types={"dst": DescriptorResultType()},
                form=DescriptorEmitForm.OP,
            ),
            EmitDescriptorOp(
                descriptor=narrow,
                operands={
                    "src": ValueRef.temporary("wide"),
                    "su": ValueRef.temporary("downshift"),
                },
                results={"dst": ValueRef.result("result")},
                form=DescriptorEmitForm.OP,
            ),
        ),
        report_key="native_"
        + source_op.name.removeprefix("vector.")
        + "_i32x16_uniform",
    )


def _f32_to_bf16_vector_rule(lane_count: int) -> DescriptorRule:
    set_rounding = _descriptor("amd.xdna.aie2p.state.rounding.immediate")
    convert = _descriptor(
        f"amd.xdna.aie2p.convert.f32x{lane_count}.to.bf16x{lane_count}"
    )
    source_type = _exact_vector("f32", lane_count)
    result_type = _exact_vector("bf16", lane_count)
    native_source = ValueRef.operand("input")
    input_emits: tuple[ContractEmit, ...] = ()
    native_result = ValueRef.result("result")
    result_types = None
    output_emits: tuple[ContractEmit, ...] = ()
    if lane_count == 16:
        native_source = ValueRef.temporary("source_accumulator")
        native_result = ValueRef.temporary("converted_w")
        result_types = {"dst": DescriptorResultType()}
        input_emits = (
            EmitDescriptorOp(
                descriptor=_descriptor(
                    "amd.xdna.aie2p.move.vector512.to.accumulator512"
                ),
                operands={"src": ValueRef.operand("input")},
                results={"dst": native_source},
                result_types={"dst": DescriptorResultType()},
                form=DescriptorEmitForm.OP,
            ),
        )
        output_emits = (
            EmitRegisterSlice(
                source=ValueRef.operand("input"),
                result=ValueRef.temporary("unused_w"),
                unit_offset=1,
                unit_count=1,
            ),
            EmitRegisterConcat(
                sources=(native_result, ValueRef.temporary("unused_w")),
                result=ValueRef.result("result"),
            ),
        )
    return DescriptorRule(
        source_op=vector.vector_fptrunc,
        descriptor=convert,
        guards=(
            Guard.value_type("input", source_type),
            Guard.value_type("result", result_type),
        ),
        emit=(
            *input_emits,
            EmitDescriptorOp(
                descriptor=set_rounding,
                immediates={"i": BF16_CONVERSION_ROUNDING},
                form=DescriptorEmitForm.OP,
            ),
            EmitDescriptorOp(
                descriptor=convert,
                operands={"src": native_source},
                results={"dst": native_result},
                result_types=result_types,
                form=DescriptorEmitForm.OP,
            ),
            *output_emits,
        ),
        report_key=f"native_binary32x{lane_count}_to_bfloat16x{lane_count}",
    )


def _bf16_to_f32_vector_rule(lane_count: int) -> DescriptorRule:
    convert = _descriptor(
        f"amd.xdna.aie2p.convert.bf16x{lane_count}.to.f32x{lane_count}"
    )
    source_type = _exact_vector("bf16", lane_count)
    result_type = _exact_vector("f32", lane_count)
    native_source = ValueRef.operand("input")
    input_emits: tuple[ContractEmit, ...] = ()
    native_result = ValueRef.result("result")
    result_types = None
    output_emits: tuple[ContractEmit, ...] = ()
    if lane_count == 16:
        native_source = ValueRef.temporary("source_w")
        native_result = ValueRef.temporary("converted_accumulator")
        result_types = {"dst": DescriptorResultType()}
        input_emits = (
            EmitRegisterSlice(
                source=ValueRef.operand("input"),
                result=native_source,
                unit_count=1,
            ),
        )
        output_emits = (
            EmitDescriptorOp(
                descriptor=_descriptor(
                    "amd.xdna.aie2p.move.accumulator512.to.vector512"
                ),
                operands={"src": native_result},
                results={"dst": ValueRef.result("result")},
                form=DescriptorEmitForm.OP,
            ),
        )
    return DescriptorRule(
        source_op=vector.vector_extf,
        descriptor=convert,
        guards=(
            Guard.value_type("input", source_type),
            Guard.value_type("result", result_type),
        ),
        emit=(
            *input_emits,
            EmitDescriptorOp(
                descriptor=convert,
                operands={"src": native_source},
                results={"dst": native_result},
                result_types=result_types,
                form=DescriptorEmitForm.OP,
            ),
            *output_emits,
        ),
        report_key=f"native_bfloat16x{lane_count}_to_binary32x{lane_count}",
    )


def _integer_pack_emits(
    pack_case: IntegerPackCase,
    pack: Descriptor,
    source: ValueRef,
    *,
    saturation: int = 0,
) -> tuple[ContractEmit, ...]:
    packed_result = (
        ValueRef.temporary("packed_w")
        if pack_case.pad_result
        else ValueRef.result("result")
    )
    result_emits: tuple[ContractEmit, ...] = ()
    if pack_case.pad_result:
        result_emits = (
            EmitRegisterSlice(
                source=source,
                result=ValueRef.temporary("unused_w"),
                unit_offset=1,
                unit_count=1,
            ),
            EmitRegisterConcat(
                sources=(packed_result, ValueRef.temporary("unused_w")),
                result=ValueRef.result("result"),
            ),
        )
    return (
        *integer_pack_state_emits(pack_case.pack_size, saturation=saturation),
        EmitDescriptorOp(
            descriptor=pack,
            operands={"src": source},
            results={"dst": packed_result},
            result_types=(
                {"dst": DescriptorResultType()} if pack_case.pad_result else None
            ),
            form=DescriptorEmitForm.OP,
        ),
        *result_emits,
    )


def _integer_pack_rule(pack_case: IntegerPackCase) -> DescriptorRule:
    pack = _descriptor(
        f"amd.xdna.aie2p.pack.{pack_case.physical_width}.trunc.configured"
    )
    return DescriptorRule(
        source_op=pack_case.source_op,
        descriptor=pack,
        guards=(
            Guard.value_type(
                pack_case.source_field,
                _exact_vector(pack_case.input_element, pack_case.input_lanes),
            ),
            Guard.value_type("result", _exact_vector("i8", pack_case.result_lanes)),
            *(
                (
                    Guard.attr_kind("width", "i64"),
                    Guard.i64_range(
                        "width",
                        pack_case.bit_width,
                        pack_case.bit_width,
                    ),
                )
                if pack_case.bit_width is not None
                else ()
            ),
        ),
        emit=_integer_pack_emits(
            pack_case, pack, ValueRef.operand(pack_case.source_field)
        ),
        report_key=pack_case.report_key,
    )


def _saturating_i4_pack_rule(
    pack_case: IntegerPackCase,
    outer_op: Op,
    inner_op: Op,
    value_fields: tuple[str, str],
) -> DescriptorRule:
    # The shared matcher requires each consumed clamp result to be adjacent and
    # single-use. The original input and bound constants may have other users.
    bounds = {vector.vector_maxsi: -8, vector.vector_minsi: 7}
    bound_fields = tuple("rhs" if field == "lhs" else "lhs" for field in value_fields)
    pack = _descriptor(
        f"amd.xdna.aie2p.pack.{pack_case.physical_width}.signed.configured"
    )
    source = ValueRef.operand(value_fields[1], source_node="inner")
    order = "min_max" if outer_op is vector.vector_minsi else "max_min"
    return DescriptorRule(
        source_op=vector.vector_bitpack,
        descriptor=pack,
        priority=1,
        guards=(
            Guard.value_type("source", _exact_vector("i8", pack_case.input_lanes)),
            Guard.value_type("result", _exact_vector("i8", pack_case.result_lanes)),
            Guard.i64_range("width", 4, 4),
        ),
        source_nodes=(
            SourceNode.adjacent_definition(
                "outer",
                source_op=outer_op,
                parent_operand=ValueRef.operand("source"),
                node_result=ValueRef.result("result"),
                guards=(
                    Guard.value_exact_i64(bound_fields[0]),
                    Guard.value_i64_range(
                        bound_fields[0], bounds[outer_op], bounds[outer_op]
                    ),
                ),
            ),
            SourceNode.adjacent_definition(
                "inner",
                source_op=inner_op,
                parent="outer",
                parent_operand=ValueRef.operand(value_fields[0]),
                node_result=ValueRef.result("result"),
                guards=(
                    Guard.value_exact_i64(bound_fields[1]),
                    Guard.value_i64_range(
                        bound_fields[1], bounds[inner_op], bounds[inner_op]
                    ),
                ),
            ),
        ),
        emit=_integer_pack_emits(pack_case, pack, source, saturation=1),
        report_key=(
            f"native_saturating_signed_i8x{pack_case.input_lanes}_to_i4_"
            f"{order}_{value_fields[0]}_{value_fields[1]}"
        ),
    )


AIE2P_PACKET_CONVERSION_RULES = (
    *(
        _saturating_i4_pack_rule(pack_case, outer_op, inner_op, value_fields)
        for pack_case in INTEGER_PACK_CASES
        if pack_case.bit_width == 4
        for outer_op, inner_op in (
            (vector.vector_minsi, vector.vector_maxsi),
            (vector.vector_maxsi, vector.vector_minsi),
        )
        for value_fields in product(("lhs", "rhs"), repeat=2)
    ),
    *(
        _integer_shift_rule(source_op)
        for source_op in (vector.vector_shli, vector.vector_shrui, vector.vector_shrsi)
    ),
    *(
        _integer_bitunpack_rule(source_op, source_kind, source_lane_count)
        for source_op, source_kind in (
            (vector.vector_bitunpacku, "u"),
            (vector.vector_bitunpacks, "s"),
        )
        for source_lane_count in I4_UNPACK_SOURCE_LANE_COUNTS
    ),
    *(
        _integer_widen_rule(source_op, signedness, rule_shape)
        for source_op, signedness in (
            (vector.vector_extui, "unsigned"),
            (vector.vector_extsi, "signed"),
        )
        for rule_shape in INTEGER_WIDEN_RULE_SHAPES
    ),
    *(_integer_pack_rule(pack_case) for pack_case in INTEGER_PACK_CASES),
    *(
        _f32_to_bf16_vector_rule(lane_count)
        for lane_count in BF16_F32_PACKET_LANE_COUNTS
    ),
    *(
        _bf16_to_f32_vector_rule(lane_count)
        for lane_count in BF16_F32_PACKET_LANE_COUNTS
    ),
)
