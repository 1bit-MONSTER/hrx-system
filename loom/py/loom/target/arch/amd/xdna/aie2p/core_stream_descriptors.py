# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""AIE2P scalar and cascade stream protocol descriptors."""

from __future__ import annotations

from dataclasses import replace

from loom.target.arch.amd.xdna.aie2p.core_descriptor_spec import _DescriptorSpec
from loom.target.low_descriptors import DescriptorFlag, Effect, EffectKind

_TARGET_KEY = "amd.xdna.aie2p"
_SCALAR_STREAM_ISSUE_EVENT = f"{_TARGET_KEY}.scalar-stream.issue"
_SCALAR_STREAM_EFFECT = Effect(
    EffectKind.BARRIER,
    producer_event=_SCALAR_STREAM_ISSUE_EVENT,
    consumer_event=_SCALAR_STREAM_ISSUE_EVENT,
)


def _scalar_stream_descriptor_specs() -> tuple[_DescriptorSpec, ...]:
    """Exposes scalar streams, packet headers, and their completion status."""

    # Stream traffic participates in a device protocol even when the payload
    # has no SSA consumer. Preserve its issue order with memory and other
    # protocol effects; this annotation does not add a hardware memory fence.
    # Consecutive scalar transfers can issue every cycle. The independent
    # srMS0/srSS0 register dependencies protect observations of their status.
    effects = (_SCALAR_STREAM_EFFECT,)
    result = []
    for nonblocking in (False, True):
        form_infix = "nb_" if nonblocking else ""
        key_suffix = ".nonblocking" if nonblocking else ""
        mnemonic_suffix = ".nb" if nonblocking else ""
        result.append(
            _DescriptorSpec(
                f"MOV_{form_infix}lda",
                f"{_TARGET_KEY}.stream.read{key_suffix}.i32",
                f"stream.read{key_suffix}.i32",
                f"II_MOV_{form_infix}lda",
                asm_mnemonic=f"mov.ss{mnemonic_suffix}",
                effects=effects,
                flags=(DescriptorFlag.BARRIER,),
            )
        )
        for form_suffix, last_key, last_mnemonic in (
            ("mMStream_tlast_imm", "", ""),
            ("mMStream_tlast_reg", ".last.register", ".last.reg"),
            ("tlast", ".last", ".last"),
        ):
            for register_class in ("eR", "eP", "eDC", "eDJ", "eDN", "eM"):
                stem = "st_" if form_suffix.startswith("mMStream") else ""
                form = f"MOV_{form_infix}{stem}{form_suffix}"
                storage_key = (
                    "i32" if register_class == "eR" else register_class.lower()
                )
                storage_mnemonic = "" if register_class == "eR" else f".{storage_key}"
                key = f"stream.write{key_suffix}{last_key}.{storage_key}"
                result.append(
                    _DescriptorSpec(
                        form,
                        f"{_TARGET_KEY}.{key}",
                        key,
                        f"II_{form}_{register_class}",
                        storage_overrides=(("src", register_class),),
                        asm_mnemonic=f"mov.ms{mnemonic_suffix}{last_mnemonic}{storage_mnemonic}",
                        effects=effects,
                        flags=(DescriptorFlag.BARRIER,),
                    )
                )
            for header, operation in (("PH", "packet"), ("CPH", "control-packet")):
                form = f"MOV_{header}_{form_infix}{form_suffix}"
                key = f"stream.write.{operation}{key_suffix}{last_key}"
                result.append(
                    _DescriptorSpec(
                        form,
                        f"{_TARGET_KEY}.{key}",
                        key,
                        f"II_{form}",
                        asm_mnemonic=f"mov.{header.lower()}{mnemonic_suffix}{last_mnemonic}",
                        effects=effects,
                        flags=(DescriptorFlag.BARRIER,),
                    )
                )
    for direction, port, register_class in (
        ("read", "ss", "mSRSS0"),
        ("write", "ms", "mSRMS0"),
    ):
        result.append(
            _DescriptorSpec(
                "MOV_alu_mv_mv_mv_scl",
                f"{_TARGET_KEY}.stream.{direction}.status",
                f"stream.{direction}.status",
                f"II_MOV_alu_mv_mv_mv_scl_eR_{register_class}",
                storage_overrides=(("dst", "eR"), ("src", register_class)),
                implicit_inputs=("src",),
                asm_mnemonic=f"mov.{port}.status",
            )
        )
    return tuple(result)


def _cascade_descriptor_specs() -> tuple[_DescriptorSpec, ...]:
    """Exposes native transfers, accumulator arithmetic, and enable state."""

    result = []
    for direction, port, native_stem, register_class in (
        ("read", "scd", "lda_mv_scd", "mCRSCDEn"),
        ("write", "mcd", "st_mv_mcd", "mCRMCDEn"),
    ):
        result.append(
            _DescriptorSpec(
                "MOV_alu_mv_mv_mv_cg",
                f"{_TARGET_KEY}.state.{port}-enable.immediate",
                f"state.write.{port}-enable",
                f"II_MOV_alu_mv_mv_mv_cg_{register_class}",
                storage_overrides=(("dst", register_class),),
                implicit_outputs=("dst",),
                asm_mnemonic=f"set.{port}-enable",
            )
        )
        for payload, suffix, mnemonic_suffix in (
            ("vector", "x", ""),
            ("accumulator", "bm", ".acc"),
        ):
            form = f"VMOV_{native_stem}_{suffix}"
            key = f"cascade.{direction}.{payload}.512"
            result.append(
                _DescriptorSpec(
                    form,
                    f"{_TARGET_KEY}.{key}",
                    key,
                    f"II_{form}",
                    storage_overrides=(
                        (("dst", "mBMs"),)
                        if direction == "read" and payload == "accumulator"
                        else ()
                    ),
                    asm_mnemonic=f"vmov.{port}{mnemonic_suffix}",
                    # Each transfer consumes or produces a distinct stream
                    # value, even when its payload has no SSA consumer.
                    effects=(Effect(EffectKind.BARRIER),),
                    flags=(DescriptorFlag.BARRIER,),
                )
            )
    # Expansion produces a fresh accumulator with the incoming value in one
    # 512-bit slot and zeros elsewhere. Register selectors outside 0..3 yield
    # zero without consuming a stream value. Increment updates the full r31
    # value modulo 2^32; it does not wrap the quarter index modulo four.
    for form, suffix in (
        ("VMOV_0_mv_scd_cm", "1024.0"),
        ("VMOV_1_mv_scd_cm", "1024.1"),
        ("VMOV_0_mv_scd_dm_imm", "2048.0"),
        ("VMOV_1_mv_scd_dm_imm", "2048.1"),
        ("VMOV_2", "2048.2"),
        ("VMOV_3", "2048.3"),
        ("VMOV_lda_mv_scd_dm_reg", "2048"),
        ("VMOV_lda_mv_scd_dm_dyn", "2048.increment"),
    ):
        key = f"cascade.read.expand.{suffix}"
        result.append(
            _DescriptorSpec(
                form,
                f"{_TARGET_KEY}.{key}",
                key,
                f"II_{form}",
                storage_overrides=(("dst", "mBMs"),),
                asm_mnemonic=f"vmov.scd.expand.{suffix}",
                effects=(Effect(EffectKind.BARRIER),),
                flags=(DescriptorFlag.BARRIER,),
            )
        )
    for operation in ("add", "sub"):
        for payload, form_infix in (("integer", ""), ("floating", "_f")):
            for increment in (False, True):
                suffix = ".increment" if increment else ""
                form = f"V{operation.upper()}{form_infix}_vmac_cm2_add_scd"
                if increment:
                    form += "_incr"
                key = f"cascade.{operation}.{payload}.configured{suffix}"
                result.append(
                    _DescriptorSpec(
                        form,
                        f"{_TARGET_KEY}.{key}",
                        key,
                        f"II_{form}",
                        storage_overrides=(("dst", "mBMs"), ("acc1", "mBMs")),
                        asm_mnemonic=f"v{operation}.acc.{payload}.scd{suffix}",
                        effects=(Effect(EffectKind.BARRIER),),
                        flags=(DescriptorFlag.BARRIER,),
                    )
                )
    return tuple(result)


def _cascade_matrix_descriptor_specs(
    matrix_specs: tuple[_DescriptorSpec, ...],
) -> tuple[_DescriptorSpec, ...]:
    """Replaces the second matrix accumulator with a selected cascade item."""

    result = []
    for spec in matrix_specs:
        if "_add_reg_" not in spec.form_name:
            continue
        for increment in (False, True):
            suffix = ".increment" if increment else ""
            form = spec.form_name.replace(
                "_add_reg_", "_add_scd_incr_" if increment else "_add_scd_"
            )
            result.append(
                replace(
                    spec,
                    form_name=form,
                    itinerary=f"II_{form}",
                    key=spec.key.replace(".matrix.", ".cascade.matrix.") + suffix,
                    semantic_tag="cascade." + spec.semantic_tag + suffix,
                    storage_overrides=tuple(
                        entry for entry in spec.storage_overrides if entry[0] != "acc2"
                    ),
                    asm_mnemonic=f"{spec.asm_mnemonic}.scd{suffix}",
                    effects=(*spec.effects, Effect(EffectKind.BARRIER)),
                    flags=(*spec.flags, DescriptorFlag.BARRIER),
                )
            )
    return tuple(result)
