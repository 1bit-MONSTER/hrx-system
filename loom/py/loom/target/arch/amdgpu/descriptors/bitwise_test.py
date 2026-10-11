# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Bit-operation descriptor coverage across native AMDGPU families."""

import pytest

from loom.target.arch.amdgpu.descriptors import (
    _REG_PART_SGPR_LOW16,
    _REG_PART_VGPR_LOW16,
    _REG_SGPR,
    _REG_VGPR,
)
from loom.target.arch.amdgpu.descriptors.api import (
    _AMDGPU_CORE_DESCRIPTOR_SET_BUILDERS,
)
from loom.target.low_descriptors import ImmediateFlag


@pytest.mark.parametrize("target", _AMDGPU_CORE_DESCRIPTOR_SET_BUILDERS)
def test_index_bit_operation_descriptors_cover_every_family(target) -> None:
    overlays = {
        overlay.descriptor_key: overlay
        for overlay in _AMDGPU_CORE_DESCRIPTOR_SET_BUILDERS[target].overlay_rows()
    }
    for key, semantic in (
        ("s_clz_i32_u32", "integer.ctlz.u32.native_zero_minus_one"),
        ("v_clz_i32_u32", "integer.ctlz.u32.native_zero_minus_one"),
        ("v_cls_i32", "integer.cls.i32.native_all_sign_minus_one"),
        ("s_ctz_i32_b32", "integer.cttz.u32.native_zero_minus_one"),
        ("v_ctz_i32_b32", "integer.cttz.u32.native_zero_minus_one"),
        ("s_bcnt1_i32_b32", "integer.ctpop.u32"),
        ("v_bcnt_u32_b32.src1_zero", "integer.ctpop.u32"),
        ("v_alignbit_b32", "integer.alignbit.u32"),
    ):
        assert overlays[f"amdgpu.{key}"].semantic_tag == semantic
    cdna = target.startswith(("cdna", "gfx9_4"))
    assert overlays["amdgpu.s_clz_i32_u32"].instruction_name == (
        "S_FLBIT_I32_B32" if cdna else "S_CLZ_I32_U32"
    )
    assert overlays["amdgpu.v_clz_i32_u32"].instruction_name == (
        "V_FFBH_U32" if cdna else "V_CLZ_I32_U32"
    )
    assert overlays["amdgpu.v_cls_i32"].instruction_name == (
        "V_FFBH_I32" if cdna else "V_CLS_I32"
    )


@pytest.mark.parametrize("target", _AMDGPU_CORE_DESCRIPTOR_SET_BUILDERS)
@pytest.mark.parametrize("type_suffix", ["i32", "u32"])
def test_low16_bitfield_extracts_cover_signedness_and_widths(
    target, type_suffix: str
) -> None:
    overlays = {
        overlay.descriptor_key: overlay
        for overlay in _AMDGPU_CORE_DESCRIPTOR_SET_BUILDERS[target].overlay_rows()
    }
    descriptor = overlays[f"amdgpu.v_bfe_{type_suffix}.offset_0_width_inline_low16"]

    result = descriptor.operands[0].descriptor_operand
    source = descriptor.operands[1].descriptor_operand
    assert result.reg_alts[0].register_part is None
    assert {
        alternative.reg_class: alternative.register_part
        for alternative in source.reg_alts
    } == {
        _REG_SGPR: _REG_PART_SGPR_LOW16,
        _REG_VGPR: _REG_PART_VGPR_LOW16,
    }

    offset, width = descriptor.immediates
    assert offset.field_name == "offset"
    assert offset.flags == (ImmediateFlag.DEFAULT_VALUE,)
    assert offset.default_value == 0
    assert offset.unsigned_max == 0
    assert width.field_name == "width"
    assert width.flags == (ImmediateFlag.DEFAULT_VALUE,)
    assert width.default_value == 16
    assert width.unsigned_max == 16
