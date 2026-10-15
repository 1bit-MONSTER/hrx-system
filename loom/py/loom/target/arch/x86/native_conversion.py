# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Native integer and floating-point conversion instruction families."""

from loom.target.arch.x86.vector_encoding import (
    VectorEncoding,
    VectorEncodingPrefix,
    VectorMachineInstruction,
    VectorOpcodeMap,
    VectorOperandShape,
    VectorRegisterSelector,
    vector_encoding_recipe,
)
from loom.target.low_descriptors import OperandRole


def float_widening_instruction(*, scalar: bool) -> VectorMachineInstruction:
    mnemonic = "vcvtss2sd" if scalar else "vcvtps2pd"
    return VectorMachineInstruction(
        descriptor_mnemonic=mnemonic,
        encoding_mnemonic=mnemonic,
        encoding_format_id=vector_encoding_recipe(
            VectorRegisterSelector.RESULT,
            VectorRegisterSelector.INPUT_0 if scalar else VectorRegisterSelector.NONE,
            VectorRegisterSelector.INPUT_1
            if scalar
            else VectorRegisterSelector.INPUT_0,
        ),
        operands=(
            VectorOperandShape(OperandRole.RESULT, ("x86.simd",)),
            *(
                (VectorOperandShape(OperandRole.OPERAND, ("x86.simd",)),)
                if scalar
                else ()
            ),
            VectorOperandShape(OperandRole.OPERAND, ("x86.simd",)),
        ),
        immediates=(),
        encodings=tuple(
            VectorEncoding(
                prefix=prefix,
                opcode_map=VectorOpcodeMap.MAP_0F,
                mandatory_prefix=2 if scalar else 0,
                w=0,
                opcode=0x5A,
                vector_bit_widths=(128,) if scalar else widths,
            )
            for prefix, widths in (
                (VectorEncodingPrefix.VEX, (128, 256)),
                (VectorEncodingPrefix.EVEX, (128, 256, 512)),
            )
        ),
    )


def integer_float_instruction(
    integer_bit_width: int, float_bit_width: int, *, unsigned: bool
) -> VectorMachineInstruction:
    if integer_bit_width not in (32, 64) or float_bit_width not in (32, 64):
        raise ValueError("scalar integer conversion requires 32/64-bit operands")
    mnemonic = ("vcvtusi2" if unsigned else "vcvtsi2") + (
        "ss" if float_bit_width == 32 else "sd"
    )
    return VectorMachineInstruction(
        descriptor_mnemonic=mnemonic,
        encoding_mnemonic=mnemonic,
        encoding_format_id=vector_encoding_recipe(
            VectorRegisterSelector.RESULT,
            VectorRegisterSelector.INPUT_0,
            VectorRegisterSelector.INPUT_1,
        ),
        operands=(
            VectorOperandShape(OperandRole.RESULT, ("x86.simd",)),
            VectorOperandShape(OperandRole.OPERAND, ("x86.simd",)),
            VectorOperandShape(OperandRole.OPERAND, (f"x86.gpr{integer_bit_width}",)),
        ),
        immediates=(),
        encodings=(
            VectorEncoding(
                prefix=(
                    VectorEncodingPrefix.EVEX if unsigned else VectorEncodingPrefix.VEX
                ),
                opcode_map=VectorOpcodeMap.MAP_0F,
                mandatory_prefix=2 if float_bit_width == 32 else 3,
                w=int(integer_bit_width == 64),
                opcode=0x7B if unsigned else 0x2A,
                vector_bit_widths=(128,),
            ),
        ),
    )
