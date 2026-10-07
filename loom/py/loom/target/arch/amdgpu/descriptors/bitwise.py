# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

# ruff: noqa: F403, F405

"""Integer bit-count and rotation descriptor overlays."""

from __future__ import annotations

from .common import *


def _s_bcnt1_i32_overlay(
    source_bit_count: int, encoding_condition: str
) -> AmdgpuDescriptorOverlay:
    if source_bit_count not in (32, 64):
        raise ValueError("S_BCNT1_I32 source width must be 32 or 64")
    return AmdgpuDescriptorOverlay(
        descriptor_key=f"amdgpu.s_bcnt1_i32_b{source_bit_count}",
        instruction_name=f"S_BCNT1_I32_B{source_bit_count}",
        mnemonic=f"s_bcnt1_i32_b{source_bit_count}",
        encoding_name="ENC_SOP1",
        encoding_condition=encoding_condition,
        semantic_tag=f"integer.ctpop.u{source_bit_count}",
        schedule_class=_SCHEDULE_SALU,
        operands=(
            AmdgpuOperandOverlay("SDST", _sgpr_result()),
            AmdgpuOperandOverlay(
                "SSRC0", _sgpr_operand("input", units=source_bit_count // 32)
            ),
        ),
        implicit_operands=(_SCC_CLOBBER_OUTPUT,),
        flags=(DescriptorFlag.DEAD_REMOVABLE,),
    )


def _s_ctz_i32_overlay(
    source_bit_count: int,
    encoding_condition: str,
    instruction_name: str,
) -> AmdgpuDescriptorOverlay:
    if source_bit_count not in (32, 64):
        raise ValueError("scalar CTZ source width must be 32 or 64")
    return AmdgpuDescriptorOverlay(
        descriptor_key=f"amdgpu.s_ctz_i32_b{source_bit_count}",
        instruction_name=instruction_name,
        mnemonic=instruction_name.lower(),
        encoding_name="ENC_SOP1",
        encoding_condition=encoding_condition,
        semantic_tag=f"integer.cttz.u{source_bit_count}.native_zero_minus_one",
        schedule_class=_SCHEDULE_SALU,
        operands=(
            AmdgpuOperandOverlay("SDST", _sgpr_result()),
            AmdgpuOperandOverlay(
                "SSRC0", _sgpr_operand("input", units=source_bit_count // 32)
            ),
        ),
        flags=(DescriptorFlag.DEAD_REMOVABLE,),
    )


def _v_bcnt_u32_b32_overlay() -> AmdgpuDescriptorOverlay:
    return AmdgpuDescriptorOverlay(
        descriptor_key="amdgpu.v_bcnt_u32_b32",
        instruction_name="V_BCNT_U32_B32",
        mnemonic="v_bcnt_u32_b32",
        encoding_name="ENC_VOP3",
        semantic_tag="integer.ctpop.accumulate.u32",
        schedule_class=_SCHEDULE_VALU,
        operands=(
            AmdgpuOperandOverlay("VDST", _vgpr_result()),
            AmdgpuOperandOverlay("SRC0", _sgpr_vgpr_operand("input")),
            AmdgpuOperandOverlay("SRC1", _sgpr_vgpr_operand("addend")),
        ),
        flags=(DescriptorFlag.DEAD_REMOVABLE,),
    )


def _v_ctz_i32_b32_overlay(instruction_name: str) -> AmdgpuDescriptorOverlay:
    return AmdgpuDescriptorOverlay(
        descriptor_key="amdgpu.v_ctz_i32_b32",
        instruction_name=instruction_name,
        mnemonic=instruction_name.lower(),
        encoding_name="ENC_VOP1",
        semantic_tag="integer.cttz.u32.native_zero_minus_one",
        schedule_class=_SCHEDULE_VALU,
        operands=(
            AmdgpuOperandOverlay("VDST", _vgpr_result()),
            AmdgpuOperandOverlay("SRC0", _sgpr_vgpr_operand("input")),
        ),
        flags=(DescriptorFlag.DEAD_REMOVABLE,),
    )


def _v_bcnt_u32_b32_src1_zero_overlay() -> AmdgpuDescriptorOverlay:
    return AmdgpuDescriptorOverlay(
        descriptor_key="amdgpu.v_bcnt_u32_b32.src1_zero",
        instruction_name="V_BCNT_U32_B32",
        mnemonic="v_bcnt_u32_b32_src1_zero",
        encoding_name="ENC_VOP3",
        semantic_tag="integer.ctpop.u32",
        schedule_class=_SCHEDULE_VALU,
        operands=(
            AmdgpuOperandOverlay("VDST", _vgpr_result()),
            AmdgpuOperandOverlay("SRC0", _sgpr_vgpr_operand("input")),
        ),
        fixed_encoding_fields=(("SRC1", _predefined("0", "OPR_SRC")),),
        flags=(DescriptorFlag.DEAD_REMOVABLE,),
    )


def _integer_bit_count_overlays(
    sop1_encoding_condition: str = "default",
    *,
    scalar_ctz_instruction_names: tuple[str, str] = (
        "S_FF1_I32_B32",
        "S_FF1_I32_B64",
    ),
    vector_ctz_instruction_name: str = "V_FFBL_B32",
    scalar_clz_instruction_name: str = "S_FLBIT_I32_B32",
    vector_clz_instruction_name: str = "V_FFBH_U32",
) -> tuple[AmdgpuDescriptorOverlay, ...]:
    return (
        _s_bcnt1_i32_overlay(32, sop1_encoding_condition),
        _s_bcnt1_i32_overlay(64, sop1_encoding_condition),
        _s_ctz_i32_overlay(
            32, sop1_encoding_condition, scalar_ctz_instruction_names[0]
        ),
        _s_ctz_i32_overlay(
            64, sop1_encoding_condition, scalar_ctz_instruction_names[1]
        ),
        _v_bcnt_u32_b32_overlay(),
        _v_bcnt_u32_b32_src1_zero_overlay(),
        _v_ctz_i32_b32_overlay(vector_ctz_instruction_name),
        replace(
            _s_ctz_i32_overlay(
                32, sop1_encoding_condition, scalar_clz_instruction_name
            ),
            descriptor_key="amdgpu.s_clz_i32_u32",
            semantic_tag="integer.ctlz.u32.native_zero_minus_one",
        ),
        replace(
            _v_ctz_i32_b32_overlay(vector_clz_instruction_name),
            descriptor_key="amdgpu.v_clz_i32_u32",
            semantic_tag="integer.ctlz.u32.native_zero_minus_one",
        ),
    )


def _rdna_integer_bit_count_overlays(
    sop1_encoding_condition: str,
) -> tuple[AmdgpuDescriptorOverlay, ...]:
    return _integer_bit_count_overlays(
        sop1_encoding_condition,
        scalar_ctz_instruction_names=("S_CTZ_I32_B32", "S_CTZ_I32_B64"),
        vector_ctz_instruction_name="V_CTZ_I32_B32",
        scalar_clz_instruction_name="S_CLZ_I32_U32",
        vector_clz_instruction_name="V_CLZ_I32_U32",
    )


def _v_alignbit_b32_overlay() -> AmdgpuDescriptorOverlay:
    return AmdgpuDescriptorOverlay(
        descriptor_key="amdgpu.v_alignbit_b32",
        instruction_name="V_ALIGNBIT_B32",
        mnemonic="v_alignbit_b32",
        encoding_name="ENC_VOP3",
        semantic_tag="integer.alignbit.u32",
        schedule_class=_SCHEDULE_VALU,
        operands=(
            AmdgpuOperandOverlay("VDST", _vgpr_result()),
            AmdgpuOperandOverlay("SRC0", _sgpr_vgpr_operand("high")),
            AmdgpuOperandOverlay("SRC1", _sgpr_vgpr_operand("low")),
            AmdgpuOperandOverlay(
                "SRC2",
                _sgpr_vgpr_operand("shift"),
                size_exception_reason="Low five count bits use a full register.",
            ),
        ),
        flags=(DescriptorFlag.DEAD_REMOVABLE,),
    )


__all__ = (
    "_integer_bit_count_overlays",
    "_rdna_integer_bit_count_overlays",
    "_v_alignbit_b32_overlay",
)
