# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Arithmetic descriptor coverage across native AMDGPU families."""

import pytest

from loom.target.arch.amdgpu.descriptors import _REG_SGPR, _REG_VGPR
from loom.target.arch.amdgpu.descriptors.api import (
    _AMDGPU_CORE_DESCRIPTOR_SET_BUILDERS,
)
from loom.target.low_descriptors import OperandRole, RegClassAlt


@pytest.mark.parametrize("target", _AMDGPU_CORE_DESCRIPTOR_SET_BUILDERS)
@pytest.mark.parametrize("bit_width", [32, 64])
def test_ldexp_descriptors_cover_every_family(target, bit_width: int) -> None:
    overlays = {
        overlay.descriptor_key: overlay
        for overlay in _AMDGPU_CORE_DESCRIPTOR_SET_BUILDERS[target].overlay_rows()
    }
    descriptor = overlays[f"amdgpu.v_ldexp_f{bit_width}"]

    assert descriptor.instruction_name == f"V_LDEXP_F{bit_width}"
    assert descriptor.semantic_tag == f"float.ldexp.f{bit_width}"
    result, input_value, exponent = (
        operand.descriptor_operand for operand in descriptor.operands
    )
    assert result.role is OperandRole.RESULT
    assert result.unit_count == bit_width // 32
    assert input_value.role is OperandRole.OPERAND
    assert input_value.unit_count == bit_width // 32
    assert input_value.reg_alts == (
        RegClassAlt(_REG_SGPR),
        RegClassAlt(_REG_VGPR),
    )
    assert exponent.role is OperandRole.OPERAND
    assert exponent.unit_count == 1
    assert exponent.reg_alts == (
        RegClassAlt(_REG_SGPR),
        RegClassAlt(_REG_VGPR),
    )
