# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

from dataclasses import replace

import pytest

from loom.target.arch.amdgpu.descriptors.alignment import _with_operand_alignment
from loom.target.arch.amdgpu.descriptors.api import _AMDGPU_CORE_DESCRIPTOR_SET_BUILDERS
from loom.target.arch.amdgpu.descriptors.common import _REG_SGPR, _REG_VGPR
from loom.target.arch.amdgpu.descriptors.contracts import (
    _amdgpu_contract_descriptor_from_overlay,
)
from loom.target.low_descriptors import RegClassAlt, RegClassFlag


@pytest.mark.parametrize("target", _AMDGPU_CORE_DESCRIPTOR_SET_BUILDERS)
def test_instruction_alignment_across_native_families(target) -> None:
    builder = _AMDGPU_CORE_DESCRIPTOR_SET_BUILDERS[target]
    source = replace(
        builder.base,
        descriptors=(
            *(
                _amdgpu_contract_descriptor_from_overlay(overlay)
                for overlay in builder.overlay_rows()
            ),
            *builder.extra_descriptors,
            *builder.base.descriptors,
        ),
    )
    aligned = _with_operand_alignment(source)
    assert aligned.reg_classes == source.reg_classes
    seen_pairs = seen_wide = seen_vectors = False
    for descriptor in aligned.descriptors:
        for operand in descriptor.operands:
            for alternative in operand.reg_alts:
                expected = 1
                if alternative.reg_class == _REG_SGPR:
                    if operand.unit_count == 2:
                        expected = 2
                        seen_pairs = True
                    elif operand.unit_count > 2:
                        expected = 4
                        seen_wide = True
                elif alternative.reg_class == _REG_VGPR and operand.unit_count > 1:
                    seen_vectors = True
                assert alternative.unit_alignment == expected, (
                    descriptor.key,
                    operand.field_name,
                )
    assert seen_pairs and seen_wide and seen_vectors
    vgpr = next(row for row in aligned.reg_classes if row.name == _REG_VGPR)
    assert (RegClassFlag.EVEN_ALIGNED_TUPLES in vgpr.flags) == target.startswith(
        ("cdna", "gfx9_4")
    )


def test_alignment_belongs_to_selected_register_alternative() -> None:
    builder = _AMDGPU_CORE_DESCRIPTOR_SET_BUILDERS["rdna3_5"]
    descriptor = _amdgpu_contract_descriptor_from_overlay(builder.overlay_rows()[0])
    operand = replace(
        descriptor.operands[0],
        unit_count=4,
        reg_alts=(RegClassAlt(_REG_SGPR), RegClassAlt(_REG_VGPR)),
    )
    source = replace(
        builder.base, descriptors=(replace(descriptor, operands=(operand,)),)
    )
    aligned = _with_operand_alignment(source)
    assert tuple(
        row.unit_alignment for row in aligned.descriptors[0].operands[0].reg_alts
    ) == (4, 1)
