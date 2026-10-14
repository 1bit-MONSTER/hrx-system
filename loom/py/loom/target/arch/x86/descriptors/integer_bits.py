# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Baseline x86 integer bit-scan descriptor rows."""

from loom.target.low_descriptors import Descriptor, DescriptorFlag

from .common import (
    _SCHEDULE_SCALAR,
    _asm,
    _gpr32_operand,
    _gpr32_result,
    _gpr64_operand,
    _gpr64_result,
)
from .encoding import Flag, Form, encoding


def _bit_scan_descriptor(operation: str, opcode: int, bit_count: int) -> Descriptor:
    result = _gpr32_result if bit_count == 32 else _gpr64_result
    operand = _gpr32_operand if bit_count == 32 else _gpr64_operand
    return Descriptor(
        key=f"x86.scalar.{operation}.gpr{bit_count}",
        encoding_format_id=Form.UNARY_R_RM,
        encoding_id=encoding(opcode, flags=Flag.REX_W if bit_count == 64 else 0),
        mnemonic=operation,
        semantic_tag=f"integer.bit_scan.{operation}.i{bit_count}",
        operands=(result(), operand("input")),
        asm_forms=_asm(
            mnemonic=f"{operation}.gpr{bit_count}",
            results=("dst",),
            operands=("input",),
        ),
        schedule_class=_SCHEDULE_SCALAR,
        flags=(DescriptorFlag.DEAD_REMOVABLE,),
    )


X86_SCALAR_INTEGER_BIT_DESCRIPTORS = tuple(
    _bit_scan_descriptor(operation, opcode, bit_count)
    for operation, opcode in (("bsf", 0x0FBC), ("bsr", 0x0FBD))
    for bit_count in (32, 64)
)
