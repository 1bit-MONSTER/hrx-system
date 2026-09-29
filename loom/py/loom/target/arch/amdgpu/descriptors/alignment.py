# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Instruction register alignment, independent of allocation packing policy."""

from dataclasses import replace

from loom.target.low_descriptors import DescriptorSet

from .common import _REG_SGPR


def _with_operand_alignment(descriptor_set: DescriptorSet) -> DescriptorSet:
    # Scalar instruction pairs require even bases; wider operands require
    # four-SGPR alignment. This applies to the SGPR alternative, not to a VGPR
    # alternative of the same source field. SBASE and SRSRC encoding padding
    # also imposes these requirements. Structural tuples have no instruction
    # requirement until a packet reads or writes them. CDNA vector alignment
    # is declared independently by its VGPR/AGPR register classes.
    return replace(
        descriptor_set,
        descriptors=tuple(
            replace(
                descriptor,
                operands=tuple(
                    replace(
                        operand,
                        reg_alts=tuple(
                            replace(
                                alternative,
                                unit_alignment=max(
                                    alternative.unit_alignment,
                                    2 if operand.unit_count == 2 else 4,
                                ),
                            )
                            if alternative.reg_class == _REG_SGPR
                            and operand.unit_count > 1
                            else alternative
                            for alternative in operand.reg_alts
                        ),
                    )
                    for operand in descriptor.operands
                ),
            )
            for descriptor in descriptor_set.descriptors
        ),
    )
