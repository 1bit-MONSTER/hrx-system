# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""F32-to-F64 scalar and packed conversion descriptors."""

from loom.target.arch.x86.native_conversion import float_widening_instruction
from loom.target.arch.x86.vector_encoding import VectorEncodingPrefix
from loom.target.low_descriptors import Descriptor, DescriptorFlag

from .common import (
    _SCHEDULE_VECTOR_F32_XMM,
    _SCHEDULE_VECTOR_F32_YMM,
    _SCHEDULE_VECTOR_F32_ZMM,
    _asm,
    _vector_operand,
    _vector_result,
    _xmm_operand,
    _xmm_result,
)

_REGISTER_SUFFIXES = {128: "xmm", 256: "ymm", 512: "zmm"}
_SCHEDULES = {
    128: _SCHEDULE_VECTOR_F32_XMM,
    256: _SCHEDULE_VECTOR_F32_YMM,
    512: _SCHEDULE_VECTOR_F32_ZMM,
}


def float_widening_descriptors(*, evex: bool) -> tuple[Descriptor, ...]:
    prefix = VectorEncodingPrefix.EVEX if evex else VectorEncodingPrefix.VEX
    feature = "avx512" if evex else "avx2"
    assembly_prefix = "avx512." if evex else ""
    scalar_instruction = float_widening_instruction(scalar=True)
    scalar = Descriptor(
        key=f"x86.{feature}.vcvtss2sd.xmm",
        mnemonic="vcvtss2sd",
        semantic_tag="float.extend.f32.f64",
        operands=(
            _xmm_result(),
            _xmm_operand("passthrough"),
            _xmm_operand("input"),
        ),
        asm_forms=_asm(
            mnemonic=f"{assembly_prefix}vcvtss2sd.xmm",
            results=("dst",),
            operands=("passthrough", "input"),
        ),
        schedule_class=_SCHEDULE_VECTOR_F32_XMM,
        flags=(DescriptorFlag.DEAD_REMOVABLE,),
    )
    descriptors = [scalar_instruction.bind(scalar, prefix)]
    instruction = float_widening_instruction(scalar=False)
    for result_width in (128, 256, 512) if evex else (128, 256):
        input_width = max(128, result_width // 2)
        suffix = f"{_REGISTER_SUFFIXES[result_width]}.{_REGISTER_SUFFIXES[input_width]}"
        descriptor = Descriptor(
            key=f"x86.{feature}.vcvtps2pd.{suffix}",
            mnemonic="vcvtps2pd",
            semantic_tag=f"float.extend.f32.f64x{result_width // 64}",
            operands=(
                _vector_result(result_width),
                _vector_operand(input_width, "input"),
            ),
            asm_forms=_asm(
                mnemonic=f"{assembly_prefix}vcvtps2pd.{suffix}",
                results=("dst",),
                operands=("input",),
            ),
            schedule_class=_SCHEDULES[result_width],
            flags=(DescriptorFlag.DEAD_REMOVABLE,),
        )
        descriptors.append(instruction.bind(descriptor, prefix))
    return tuple(descriptors)
