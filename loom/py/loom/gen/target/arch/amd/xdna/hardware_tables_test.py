# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

from __future__ import annotations

from loom.gen.target.arch.amd.xdna.hardware_tables import (
    emit_aie2p_target_profiles,
    emit_array_facts,
    emit_device_profiles,
    emit_register_facts,
)


def test_hardware_table_family_is_emitted_from_one_materialized_source() -> None:
    array_contents = emit_array_facts()
    register_contents = emit_register_facts()
    profile_contents = emit_device_profiles()
    aie2p_profile_contents = emit_aie2p_target_profiles()

    assert "kLoomXdnaNpu2ArrayFamily" in array_contents
    assert ".maximum_encoded_transfer_length = UINT32_C(0x00003fff)" in array_contents
    assert ".maximum_encoded_transfer_length = UINT32_C(0x0001ffff)" in array_contents
    assert ".maximum_encoded_transfer_length = UINT32_C(0xffffffff)" in array_contents
    assert ".physical_tile_count = 32" in array_contents
    assert ".dma_channel_count_per_direction = 64" in array_contents
    assert ".dma_buffer_descriptor_count = 512" in array_contents
    assert "kLoomXdnaRegisterPatterns" in register_contents
    assert "kLoomXdnaRegisterFieldCount = 410" in register_contents
    assert "loom_xdna_device_profiles" in profile_contents
    assert "0x535848414c4f0001" in profile_contents
    assert "kLoomAie2pTargetProfiles" in aie2p_profile_contents
    assert ".device_profile = &loom_xdna_device_profiles[" in aie2p_profile_contents
