# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

import pytest

from loom.target.arch.amdgpu.descriptors.api import _AMDGPU_CORE_DESCRIPTOR_SET_BUILDERS
from loom.target.arch.amdgpu.descriptors.common import (
    _REG_AGPR,
    _REG_PART_VGPR_LOW16,
    _REG_VGPR,
)
from loom.target.arch.amdgpu.descriptors.sets import _gfx12_core_overlays


def test_gfx12_vector_store_family_exposes_cache_scope() -> None:
    prefixes = (
        "amdgpu.global_store_",
        "amdgpu.buffer_store_",
        "amdgpu.flat_store_",
    )
    stores = tuple(
        overlay
        for overlay in _gfx12_core_overlays()
        if overlay.descriptor_key.startswith(prefixes)
    )
    assert stores
    assert {
        prefix
        for prefix in prefixes
        if any(store.descriptor_key.startswith(prefix) for store in stores)
    } == set(prefixes)
    for store in stores:
        assert "scope" in {immediate.field_name for immediate in store.immediates}, (
            store.descriptor_key
        )


@pytest.mark.parametrize("target", _AMDGPU_CORE_DESCRIPTOR_SET_BUILDERS)
def test_narrow_stores_read_only_the_low_register_part(target: str) -> None:
    descriptors = {
        overlay.descriptor_key: overlay
        for overlay in _AMDGPU_CORE_DESCRIPTOR_SET_BUILDERS[target].overlay_rows()
    }
    for width in (8, 16, 32):
        keys = (
            f"amdgpu.ds_write_b{width}",
            f"amdgpu.global_store_b{width}",
            f"amdgpu.global_store_b{width}_saddr",
            f"amdgpu.scratch_store_b{width}_vaddr",
            f"amdgpu.scratch_store_b{width}_offset_only",
            "amdgpu.buffer_store_dword"
            if width == 32
            else f"amdgpu.buffer_store_b{width}",
        )
        for key in keys:
            descriptor = descriptors[key]
            variants = [descriptor]
            if key.startswith("amdgpu.buffer_store"):
                variants.extend(
                    descriptors[form.replacement_descriptor]
                    for form in descriptor.operand_forms
                )
            for variant in variants:
                value = next(
                    operand.descriptor_operand
                    for operand in variant.operands
                    if operand.descriptor_operand.field_name == "value"
                )
                assert value.reg_alts[0].register_part == (
                    _REG_PART_VGPR_LOW16 if width < 32 else None
                ), variant.descriptor_key


@pytest.mark.parametrize("target", _AMDGPU_CORE_DESCRIPTOR_SET_BUILDERS)
def test_flat_store_read_parts_follow_the_selected_register_class(target: str) -> None:
    descriptors = {
        overlay.descriptor_key: overlay
        for overlay in _AMDGPU_CORE_DESCRIPTOR_SET_BUILDERS[target].overlay_rows()
    }
    for width in (8, 16, 32, 64, 96, 128):
        descriptor = descriptors[f"amdgpu.flat_store_b{width}"]
        value = next(
            operand.descriptor_operand
            for operand in descriptor.operands
            if operand.descriptor_operand.field_name == "value"
        )
        parts_by_class = {
            alternative.reg_class: alternative.register_part
            for alternative in value.reg_alts
        }
        assert parts_by_class[_REG_VGPR] == (
            _REG_PART_VGPR_LOW16 if width < 32 else None
        )
        if _REG_AGPR in parts_by_class:
            assert parts_by_class[_REG_AGPR] is None
