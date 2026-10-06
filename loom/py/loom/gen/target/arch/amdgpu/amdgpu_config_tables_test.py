# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

from __future__ import annotations

from loom.gen.support.c import CIdentifierCase, c_identifier
from loom.gen.target.arch.amdgpu import amdgpu_config_tables
from loom.target.arch.amdgpu.encoding import (
    AMDGPU_ENCODING_FIELD_IDS,
    AMDGPU_ENCODING_FIELD_NAMES,
)
from loom.target.arch.amdgpu.names import (
    amdgpu_descriptor_set_define,
    amdgpu_low_descriptor_provider_symbol,
    amdgpu_low_descriptor_storage_view_provider_symbol,
)
from loom.target.arch.amdgpu.target_info import (
    amdgpu_descriptor_set_info_by_generator_target,
    sorted_descriptor_set_infos,
)


def _encoding_field_row(name: str) -> str:
    suffix = c_identifier(name, case=CIdentifierCase.UPPER, empty="EMPTY")
    return f"LOOM_AMDGPU_ENCODING_FIELD(LOOM_AMDGPU_ENCODING_FIELD_{suffix}, {AMDGPU_ENCODING_FIELD_IDS[name]})"


def test_encoding_field_ids_fragment_is_row_data_only() -> None:
    source = amdgpu_config_tables._emit_encoding_field_ids()

    assert "typedef " not in source
    assert "enum " not in source
    assert "#ifndef " not in source
    assert "#define " not in source
    assert "#include " not in source
    assert "\nif " not in source
    assert "\nreturn " not in source

    lines = source.splitlines()
    assert len(lines) == len(AMDGPU_ENCODING_FIELD_NAMES)
    assert lines[0] == _encoding_field_row(AMDGPU_ENCODING_FIELD_NAMES[0])
    assert lines[-1] == _encoding_field_row(AMDGPU_ENCODING_FIELD_NAMES[-1])


def _provider_branch(
    portable_target: str,
    canonical_storage_target: str,
    alternate_storage_target: str,
    macro: str,
) -> str:
    portable_info = amdgpu_descriptor_set_info_by_generator_target(portable_target)
    canonical_info = amdgpu_descriptor_set_info_by_generator_target(canonical_storage_target)
    alternate_info = amdgpu_descriptor_set_info_by_generator_target(alternate_storage_target)
    portable_define = amdgpu_descriptor_set_define(portable_info.key)
    canonical_define = amdgpu_descriptor_set_define(canonical_info.key)
    alternate_define = amdgpu_descriptor_set_define(alternate_info.key)
    canonical_provider = amdgpu_low_descriptor_provider_symbol(portable_info.key)
    alternate_provider = amdgpu_low_descriptor_storage_view_provider_symbol(portable_info.key, alternate_storage_target)
    return "\n".join(
        [
            f"#if defined({portable_define}) || defined({canonical_define})",
            f"{macro}({canonical_provider})",
            f"#elif defined({alternate_define})",
            f"{macro}({alternate_provider})",
            "#endif",
        ]
    )


def test_low_registry_selects_one_portable_provider_per_storage_family() -> None:
    source = amdgpu_config_tables._emit_low_registry_tables(sorted_descriptor_set_infos())

    for portable_target, canonical_target, alternate_target in (
        ("gfx11_generic", "rdna3", "rdna3_5"),
        ("gfx9_4_generic", "cdna3", "cdna4"),
    ):
        for macro in (
            "LOOM_AMDGPU_LOW_DESCRIPTOR_PROVIDER_DECL",
            "LOOM_AMDGPU_LOW_DESCRIPTOR_PROVIDER",
        ):
            branch = _provider_branch(
                portable_target,
                canonical_target,
                alternate_target,
                macro,
            )
            assert source.count(branch) == 1
