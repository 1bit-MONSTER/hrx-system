# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""CDNA memory payload banks over shared memory instruction overlays."""

from dataclasses import replace

from loom.target.arch.amdgpu.descriptor_overlay import AmdgpuDescriptorOverlay
from loom.target.arch.amdgpu.encoding import AMDGPU_MEMORY_REGISTER_ENCODING_ADAPTERS

from .common import (
    _REG_AGPR,
    _REG_PART_AGPR_HIGH16,
    _REG_PART_AGPR_LOW16,
    _REG_PART_VGPR_HIGH16,
    _REG_PART_VGPR_LOW16,
    _REG_VGPR,
)

_ACCUMULATOR_PARTS = {
    None: None,
    _REG_PART_VGPR_LOW16: _REG_PART_AGPR_LOW16,
    _REG_PART_VGPR_HIGH16: _REG_PART_AGPR_HIGH16,
}


def _with_accumulator_memory_banks(
    overlays: tuple[AmdgpuDescriptorOverlay, ...],
) -> tuple[AmdgpuDescriptorOverlay, ...]:
    # The CDNA memory encodings select the whole payload bank with one ACC bit.
    # Addresses remain VGPRs; XML materialization validates each payload field
    # and retains the shared-bank constraint for returning atomics.
    result = []
    for overlay in overlays:
        fields = AMDGPU_MEMORY_REGISTER_ENCODING_ADAPTERS.get(overlay.encoding_name)
        if fields is None:
            result.append(overlay)
            continue
        operands = []
        for row in overlay.operands:
            operand = row.descriptor_operand
            if row.xml_field_name in fields:
                (vector,) = operand.reg_alts
                assert vector.reg_class == _REG_VGPR
                operand = replace(
                    operand,
                    reg_alts=(
                        vector,
                        replace(
                            vector,
                            reg_class=_REG_AGPR,
                            register_part=_ACCUMULATOR_PARTS[vector.register_part],
                        ),
                    ),
                )
                row = replace(row, descriptor_operand=operand)
            operands.append(row)
        result.append(replace(overlay, operands=tuple(operands)))
    return tuple(result)
