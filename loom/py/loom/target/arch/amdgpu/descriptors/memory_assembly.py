# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

# ruff: noqa: F403, F405

"""Native memory forms retain address shape and cache syntax at construction."""

from __future__ import annotations

from typing import Literal

from .common import *


def _global_memory_asm(
    *,
    mnemonic: str,
    native_assembly_mnemonic: str,
    results: tuple[str, ...] = (),
    operands: tuple[str, ...],
    immediates: tuple[str, ...],
    native_modifiers: tuple[NativeAsmValue, ...],
) -> tuple[AsmForm, ...]:
    has_saddr = "saddr" in operands
    return _asm(
        mnemonic=f"{mnemonic}_{'saddr' if has_saddr else 'vaddr'}",
        native_assembly_mnemonic=native_assembly_mnemonic,
        results=results,
        operands=operands,
        immediates=immediates,
        named_immediates=True,
        native_assembly_values=(
            *(_native_result(name) for name in results),
            _native_operand("addr"),
            *((_native_operand("value"),) if "value" in operands else ()),
            _native_operand("saddr") if has_saddr else _native_literal("off"),
            *native_modifiers,
        ),
    )


def _buffer_memory_asm(
    *,
    mnemonic: str,
    native_assembly_mnemonic: str,
    results: tuple[str, ...] = (),
    operands: tuple[str, ...],
    immediates: tuple[str, ...],
    native_modifiers: tuple[NativeAsmValue, ...],
    fixed_soffset_native_spelling: str = "0",
) -> tuple[AsmForm, ...]:
    # Atomics print the entire input tuple, including both compare/swap values.
    # Loads print their result; LDS loads have no encoded data operand.
    payload = (
        (_native_operand("value"),)
        if "value" in operands
        else tuple(_native_result(name) for name in results)
    )
    has_vaddr = "vaddr" in operands
    return _asm(
        mnemonic=mnemonic,
        native_assembly_mnemonic=(
            native_assembly_mnemonic if native_assembly_mnemonic != mnemonic else None
        ),
        results=results,
        operands=operands,
        immediates=immediates,
        named_immediates=True,
        native_assembly_values=(
            *payload,
            _native_operand("vaddr") if has_vaddr else _native_literal("off"),
            _native_operand("resource"),
            _native_operand("soffset")
            if "soffset" in operands
            else _native_literal(fixed_soffset_native_spelling),
            *((_native_modifier_literal("offen"),) if has_vaddr else ()),
            *native_modifiers,
        ),
    )


def _native_memory_modifiers(
    cache_fields: tuple[tuple[str, int], ...],
    *,
    fixed_encoding_fields: tuple[tuple[str, AmdgpuFixedEncodingValue], ...] = (),
    temporal_operation: Literal["load", "store", "atomic"],
) -> tuple[NativeAsmValue, ...]:
    fixed_fields = dict(fixed_encoding_fields)
    modifiers = [_native_amdgpu_required_named_i64_immediate("offset")]
    for name, bit_width in cache_fields:
        spelling = name.lower()
        if name in fixed_fields:
            value = fixed_fields[name]
            if value:
                modifiers.append(
                    _native_modifier_literal(
                        spelling if bit_width == 1 else f"{spelling}:{value}"
                    )
                )
        elif name == "TH":
            modifiers.append(
                _native_amdgpu_gfx12_temporal_immediate(spelling, temporal_operation)
            )
        elif name == "SCOPE":
            modifiers.append(_native_amdgpu_gfx12_scope_immediate(spelling))
        else:
            modifiers.append(
                _native_amdgpu_named_flag_immediate(spelling)
                if bit_width == 1
                else _native_amdgpu_required_named_i64_immediate(spelling)
            )
    return tuple(modifiers)
