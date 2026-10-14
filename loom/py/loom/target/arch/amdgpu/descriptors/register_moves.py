# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Native CDNA accumulator register transfers and inline initialization."""

from loom.target.arch.amdgpu.descriptor_overlay import (
    AmdgpuDescriptorOverlay,
    AmdgpuOperandOverlay,
)
from loom.target.low_descriptors import (
    Constraint,
    ConstraintKind,
    DescriptorFlag,
    DescriptorOpKind,
    Immediate,
    ImmediateKind,
    Operand,
    OperandRole,
    RegClassAlt,
)

from .common import (
    _REG_AGPR,
    _REG_SGPR,
    _REG_VGPR,
    _SCHEDULE_VALU,
    _SOURCE_INLINE_F32_IMMEDIATE,
    _SOURCE_INLINE_U32_ENCODING_ID,
    _asm,
)


def _cdna_register_move_overlays() -> tuple[AmdgpuDescriptorOverlay, ...]:
    # These opcodes fix the accumulator bank instead of exposing an ACC field.
    # READ/WRITE retain the ordinary VOP3P high-selector defaults even though
    # they move complete 32-bit values.
    overlays = []
    for mnemonic, instruction, encoding, destination, sources in (
        ("v_accvgpr_mov_b32", "V_ACCVGPR_MOV_B32", "ENC_VOP1", _REG_AGPR, (_REG_AGPR,)),
        ("v_accvgpr_read_b32", "V_ACCVGPR_READ", "ENC_VOP3P", _REG_VGPR, (_REG_AGPR,)),
        (
            "v_accvgpr_write_b32",
            "V_ACCVGPR_WRITE",
            "ENC_VOP3P",
            _REG_AGPR,
            (_REG_SGPR, _REG_VGPR),
        ),
    ):
        overlays.append(
            AmdgpuDescriptorOverlay(
                descriptor_key=f"amdgpu.{mnemonic}",
                instruction_name=instruction,
                mnemonic=mnemonic,
                encoding_name=encoding,
                semantic_tag="register.copy.b32",
                schedule_class=_SCHEDULE_VALU,
                operands=(
                    AmdgpuOperandOverlay(
                        "VDST",
                        Operand("dst", OperandRole.RESULT, (RegClassAlt(destination),)),
                    ),
                    AmdgpuOperandOverlay(
                        "SRC0",
                        Operand(
                            "src",
                            OperandRole.OPERAND,
                            tuple(RegClassAlt(source) for source in sources),
                        ),
                    ),
                ),
                fixed_encoding_fields=(("OP_SEL_HI", 7),)
                if encoding == "ENC_VOP3P"
                else (),
                asm_forms=_asm(results=("dst",), operands=("src",)),
                flags=(DescriptorFlag.DEAD_REMOVABLE, DescriptorFlag.SAFE_TO_SPECULATE),
            )
        )
    for suffix, immediate in (
        (
            "i32",
            Immediate(
                "imm32",
                ImmediateKind.SIGNED,
                bit_width=32,
                signed_min=-16,
                unsigned_max=64,
                encoding_id=_SOURCE_INLINE_U32_ENCODING_ID,
            ),
        ),
        ("f32", _SOURCE_INLINE_F32_IMMEDIATE),
    ):
        overlays.append(
            AmdgpuDescriptorOverlay(
                descriptor_key=f"amdgpu.v_accvgpr_write_b32.inline_{suffix}",
                instruction_name="V_ACCVGPR_WRITE",
                mnemonic="v_accvgpr_write_b32",
                encoding_name="ENC_VOP3P",
                semantic_tag="integer.const.u32",
                schedule_class=_SCHEDULE_VALU,
                operands=(
                    AmdgpuOperandOverlay(
                        "VDST",
                        Operand("dst", OperandRole.RESULT, (RegClassAlt(_REG_AGPR),)),
                    ),
                ),
                op_kind=DescriptorOpKind.CONST,
                asm_forms=_asm(
                    mnemonic=f"v_accvgpr_write_b32_inline_{suffix}",
                    native_assembly_mnemonic="v_accvgpr_write_b32",
                    results=("dst",),
                    immediates=("imm32",),
                ),
                immediate_fields=("SRC0",),
                immediates=(immediate,),
                fixed_encoding_fields=(("OP_SEL_HI", 7),),
                constraints=(Constraint(ConstraintKind.REMATERIALIZABLE, 0),),
                flags=(DescriptorFlag.DEAD_REMOVABLE,),
            )
        )
    return tuple(overlays)
