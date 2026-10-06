# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

from __future__ import annotations

import re
from collections.abc import Iterator
from contextlib import contextmanager
from dataclasses import replace
from pathlib import Path

from loom.gen.target.arch.amdgpu.descriptors import amdgpu_wait_packet_tables
from loom.target.arch.amdgpu.descriptors import (
    _COUNTER_ALU,
    _COUNTER_LDS,
    _COUNTER_SMEM,
    _COUNTER_VMEM_LOAD,
    _COUNTER_VMEM_STORE,
    _WAIT_COUNTER_LGKM_ENCODING_ID,
    _WAIT_COUNTER_SMEM_ENCODING_ID,
    _WAIT_COUNTER_VMEM_ENCODING_ID,
    _WAIT_COUNTER_VMEM_LOAD_ENCODING_ID,
    _WAIT_COUNTER_VMEM_STORE_ENCODING_ID,
)
from loom.target.arch.amdgpu.target_info import (
    AMDGPU_PROCESSOR_SCHEDULING_VALU_TRANS_USE_DEPCTR,
)
from loom.target.low_descriptors import (
    Descriptor,
    DescriptorSet,
    Effect,
    EffectFlag,
    EffectKind,
    Hazard,
    HazardKind,
    Immediate,
    ImmediateKind,
    LatencyKind,
    MemorySpace,
    ModelQuality,
    Operand,
    OperandRole,
    ScheduleClass,
    StorageLease,
    StorageLeaseAttachment,
    StorageLeaseKind,
    StorageLeaseReleaseScope,
)


@contextmanager
def _raises_value_error(match: str) -> Iterator[None]:
    try:
        yield
    except ValueError as exc:
        if re.search(match, str(exc)) is None:
            raise AssertionError(f"ValueError message {exc!s} did not match {match}") from exc
    else:
        raise AssertionError("expected ValueError")


def _descriptor(
    key: str,
    *,
    effects: tuple[Effect, ...],
    immediates: tuple[Immediate, ...],
) -> Descriptor:
    return Descriptor(
        key=key,
        mnemonic=None,
        semantic_tag=None,
        operands=(),
        schedule_class="test",
        effects=effects,
        immediates=immediates,
    )


def _descriptor_set(
    *descriptors: Descriptor,
    hazards: tuple[Hazard, ...] = (),
) -> DescriptorSet:
    return DescriptorSet(
        key="amdgpu.test.core",
        target_key="amdgpu",
        feature_key=None,
        c_header_path=Path("test.h"),
        c_source_path=Path("test.c"),
        header_guard="TEST_H_",
        public_header="test.h",
        function_name="test_descriptor_set",
        c_table_prefix="test",
        c_enum_prefix="TEST",
        generator_version=0,
        reg_classes=(),
        resources=(),
        schedule_classes=(
            ScheduleClass(
                "test",
                latency_kind=LatencyKind.VARIABLE,
                model_quality=ModelQuality.FALLBACK,
                hazards=hazards,
            ),
        ),
        descriptors=descriptors,
    )


def _wait_immediate(
    field_name: str,
    encoding_id: int,
    *,
    kind: ImmediateKind = ImmediateKind.UNSIGNED,
    unsigned_max: int = 63,
) -> Immediate:
    return Immediate(
        field_name,
        kind,
        encoding_id=encoding_id,
        unsigned_max=unsigned_max,
    )


def _dependency_effect(
    kind: EffectKind,
    *,
    memory_space: MemorySpace = MemorySpace.GLOBAL,
    counter_id: int = 0,
) -> Effect:
    return Effect(
        kind,
        memory_space=memory_space,
        flags=(EffectFlag.DEPENDENCY,),
        counter_id=counter_id,
        width_bits=32,
    )


def _wait_effect(counter_id: int) -> Effect:
    return Effect(EffectKind.COUNTER, counter_id=counter_id)


def test_classifies_wait_packet_descriptor_rows() -> None:
    descriptor = _descriptor(
        "amdgpu.s_waitcnt",
        effects=(_wait_effect(_COUNTER_VMEM_LOAD), _wait_effect(_COUNTER_LDS)),
        immediates=(
            _wait_immediate("vmcnt", _WAIT_COUNTER_VMEM_ENCODING_ID),
            _wait_immediate("lgkmcnt", _WAIT_COUNTER_LGKM_ENCODING_ID),
        ),
    )

    descriptor_rows, immediate_rows, descriptor_lookup_rows, range_row = amdgpu_wait_packet_tables._descriptor_set_wait_packet_rows(
        _descriptor_set(descriptor),
        descriptor_set_ordinal=2,
        descriptor_ref_key_set={"amdgpu.s_waitcnt"},
        first_descriptor=7,
        first_immediate=11,
    )

    assert len(descriptor_rows) == 1
    descriptor_row = descriptor_rows[0]
    assert descriptor_row.descriptor_set_key == "amdgpu.test.core"
    assert descriptor_row.descriptor_set_ordinal == 2
    assert descriptor_row.descriptor_ref == "LOOM_AMDGPU_DESCRIPTOR_REF_S_WAITCNT"
    assert descriptor_row.counter_count == 2
    assert descriptor_row.immediate_start == 11
    assert descriptor_row.immediate_count == 2
    assert len(immediate_rows) == 2
    assert immediate_rows[0].field_name == "vmcnt"
    assert immediate_rows[0].counter_mask == amdgpu_wait_packet_tables._counter_mask(_COUNTER_VMEM_LOAD)
    assert immediate_rows[1].field_name == "lgkmcnt"
    assert immediate_rows[1].counter_mask == amdgpu_wait_packet_tables._counter_mask(_COUNTER_LDS)
    assert descriptor_lookup_rows == (1,)
    assert range_row.first_descriptor == 7
    assert range_row.descriptor_count == 1
    assert range_row.descriptor_lookup_count == 1
    assert range_row.max_descriptor_immediate_count == 2
    assert range_row.maximum_target_counts == (62, 0, 62, 0, 0, 0, 0, 0)


def test_selects_best_wait_packet_descriptor_rows() -> None:
    combined_descriptor = amdgpu_wait_packet_tables._WaitPacketDescriptorRow(
        descriptor_set_key="amdgpu.test.core",
        descriptor_set_ordinal=0,
        descriptor_key="amdgpu.s_waitcnt",
        descriptor_ref="LOOM_AMDGPU_DESCRIPTOR_REF_S_WAITCNT",
        counter_mask=amdgpu_wait_packet_tables._counter_mask(_COUNTER_VMEM_LOAD) | amdgpu_wait_packet_tables._counter_mask(_COUNTER_LDS),
        counter_count=2,
        immediate_start=0,
        immediate_count=2,
    )
    load_descriptor = amdgpu_wait_packet_tables._WaitPacketDescriptorRow(
        descriptor_set_key="amdgpu.test.core",
        descriptor_set_ordinal=0,
        descriptor_key="amdgpu.s_wait_loadcnt",
        descriptor_ref="LOOM_AMDGPU_DESCRIPTOR_REF_S_WAIT_LOADCNT",
        counter_mask=amdgpu_wait_packet_tables._counter_mask(_COUNTER_VMEM_LOAD),
        counter_count=1,
        immediate_start=2,
        immediate_count=1,
    )
    lds_descriptor = amdgpu_wait_packet_tables._WaitPacketDescriptorRow(
        descriptor_set_key="amdgpu.test.core",
        descriptor_set_ordinal=0,
        descriptor_key="amdgpu.s_wait_lgkmcnt",
        descriptor_ref="LOOM_AMDGPU_DESCRIPTOR_REF_S_WAIT_LGKMCNT",
        counter_mask=amdgpu_wait_packet_tables._counter_mask(_COUNTER_LDS),
        counter_count=1,
        immediate_start=3,
        immediate_count=1,
    )
    rows = (combined_descriptor, load_descriptor, lds_descriptor)

    combined_mask = amdgpu_wait_packet_tables._counter_mask(_COUNTER_VMEM_LOAD) | amdgpu_wait_packet_tables._counter_mask(_COUNTER_LDS)
    descriptor_index, covered_mask = amdgpu_wait_packet_tables._best_wait_packet_descriptor_selection(
        rows,
        combined_mask,
    )
    assert descriptor_index == 0
    assert covered_mask == combined_mask

    descriptor_index, covered_mask = amdgpu_wait_packet_tables._best_wait_packet_descriptor_selection(
        rows,
        amdgpu_wait_packet_tables._counter_mask(_COUNTER_VMEM_LOAD),
    )
    assert descriptor_index == 1
    assert covered_mask == amdgpu_wait_packet_tables._counter_mask(_COUNTER_VMEM_LOAD)


def test_classifies_split_wait_packet_descriptor_rows() -> None:
    load_descriptor = _descriptor(
        "amdgpu.s_wait_loadcnt",
        effects=(_wait_effect(_COUNTER_VMEM_LOAD),),
        immediates=(_wait_immediate("loadcnt", _WAIT_COUNTER_VMEM_LOAD_ENCODING_ID),),
    )
    store_descriptor = _descriptor(
        "amdgpu.s_wait_storecnt",
        effects=(_wait_effect(_COUNTER_VMEM_STORE),),
        immediates=(_wait_immediate("storecnt", _WAIT_COUNTER_VMEM_STORE_ENCODING_ID),),
    )
    scalar_descriptor = _descriptor(
        "amdgpu.s_wait_kmcnt",
        effects=(_wait_effect(_COUNTER_SMEM),),
        immediates=(
            _wait_immediate(
                "kmcnt",
                _WAIT_COUNTER_SMEM_ENCODING_ID,
                unsigned_max=31,
            ),
        ),
    )

    descriptor_rows, immediate_rows, descriptor_lookup_rows, range_row = amdgpu_wait_packet_tables._descriptor_set_wait_packet_rows(
        _descriptor_set(load_descriptor, store_descriptor, scalar_descriptor),
        descriptor_set_ordinal=4,
        descriptor_ref_key_set={
            "amdgpu.s_wait_loadcnt",
            "amdgpu.s_wait_storecnt",
            "amdgpu.s_wait_kmcnt",
        },
        first_descriptor=8,
        first_immediate=12,
    )

    assert [row.descriptor_ref for row in descriptor_rows] == [
        "LOOM_AMDGPU_DESCRIPTOR_REF_S_WAIT_LOADCNT",
        "LOOM_AMDGPU_DESCRIPTOR_REF_S_WAIT_STORECNT",
        "LOOM_AMDGPU_DESCRIPTOR_REF_S_WAIT_KMCNT",
    ]
    assert [row.counter_mask for row in descriptor_rows] == [
        amdgpu_wait_packet_tables._counter_mask(_COUNTER_VMEM_LOAD),
        amdgpu_wait_packet_tables._counter_mask(_COUNTER_VMEM_STORE),
        amdgpu_wait_packet_tables._counter_mask(_COUNTER_SMEM),
    ]
    assert [row.immediate_start for row in descriptor_rows] == [12, 13, 14]
    assert [immediate.field_name for immediate in immediate_rows] == [
        "loadcnt",
        "storecnt",
        "kmcnt",
    ]
    assert [immediate.counter_mask for immediate in immediate_rows] == [
        amdgpu_wait_packet_tables._counter_mask(_COUNTER_VMEM_LOAD),
        amdgpu_wait_packet_tables._counter_mask(_COUNTER_VMEM_STORE),
        amdgpu_wait_packet_tables._counter_mask(_COUNTER_SMEM),
    ]
    assert descriptor_lookup_rows == (1, 2, 3)
    assert range_row.first_descriptor == 8
    assert range_row.descriptor_count == 3
    assert range_row.descriptor_lookup_count == 3
    assert range_row.max_descriptor_immediate_count == 1

    selection_rows = amdgpu_wait_packet_tables._descriptor_set_wait_packet_selection_rows("amdgpu.test.core", 4, descriptor_rows, immediate_rows)
    assert len(selection_rows) == amdgpu_wait_packet_tables._WAIT_COUNTER_MASK_COUNT
    for index, counter_id in enumerate((_COUNTER_VMEM_LOAD, _COUNTER_VMEM_STORE, _COUNTER_SMEM)):
        counter_mask = amdgpu_wait_packet_tables._counter_mask(counter_id)
        selection = selection_rows[counter_mask]
        assert selection.descriptor_set_ordinal == 4
        assert selection.descriptor_index == index
        assert selection.covered_counter_mask == counter_mask
        assert selection.full_drain_counter_mask == counter_mask


def test_effective_bounds_cover_all_encodings_of_each_counter() -> None:
    descriptors = (
        _descriptor(
            "amdgpu.s_waitcnt",
            effects=(_wait_effect(_COUNTER_VMEM_LOAD), _wait_effect(_COUNTER_LDS)),
            immediates=(
                _wait_immediate("vmcnt", _WAIT_COUNTER_VMEM_ENCODING_ID, unsigned_max=63),
                _wait_immediate("lgkmcnt", _WAIT_COUNTER_LGKM_ENCODING_ID, unsigned_max=15),
            ),
        ),
        _descriptor(
            "amdgpu.s_wait_loadcnt",
            effects=(_wait_effect(_COUNTER_VMEM_LOAD),),
            immediates=(_wait_immediate("loadcnt", _WAIT_COUNTER_VMEM_LOAD_ENCODING_ID, unsigned_max=31),),
        ),
    )
    _, _, _, range_row = amdgpu_wait_packet_tables._descriptor_set_wait_packet_rows(
        _descriptor_set(*descriptors),
        descriptor_set_ordinal=0,
        descriptor_ref_key_set={descriptor.key for descriptor in descriptors},
        first_descriptor=0,
        first_immediate=0,
    )
    assert range_row.maximum_target_counts == (30, 0, 14, 0, 0, 0, 0, 0)


def test_skips_non_counter_descriptors() -> None:
    descriptor = _descriptor(
        "amdgpu.s_nop",
        effects=(),
        immediates=(),
    )

    descriptor_rows, immediate_rows, descriptor_lookup_rows, range_row = amdgpu_wait_packet_tables._descriptor_set_wait_packet_rows(
        _descriptor_set(descriptor),
        descriptor_set_ordinal=0,
        descriptor_ref_key_set={"amdgpu.s_nop"},
        first_descriptor=0,
        first_immediate=0,
    )

    assert descriptor_rows == ()
    assert immediate_rows == ()
    assert descriptor_lookup_rows == (0,)
    assert range_row.descriptor_count == 0
    assert range_row.descriptor_lookup_count == 1
    assert range_row.max_descriptor_immediate_count == 0


def test_skips_zero_counter_effects() -> None:
    descriptor = _descriptor(
        "amdgpu.s_wait_idle",
        effects=(_wait_effect(0),),
        immediates=(),
    )

    descriptor_rows, immediate_rows, descriptor_lookup_rows, range_row = amdgpu_wait_packet_tables._descriptor_set_wait_packet_rows(
        _descriptor_set(descriptor),
        descriptor_set_ordinal=0,
        descriptor_ref_key_set={"amdgpu.s_wait_idle"},
        first_descriptor=0,
        first_immediate=0,
    )

    assert descriptor_rows == ()
    assert immediate_rows == ()
    assert descriptor_lookup_rows == (0,)
    assert range_row.descriptor_count == 0
    assert range_row.descriptor_lookup_count == 1
    assert range_row.max_descriptor_immediate_count == 0


def test_rejects_wait_packets_with_nonconcrete_counter_effects() -> None:
    descriptor = _descriptor(
        "amdgpu.s_wait_loadcnt",
        effects=(_wait_effect(_COUNTER_VMEM_LOAD),),
        immediates=(_wait_immediate("loadcnt", _WAIT_COUNTER_VMEM_LOAD_ENCODING_ID),),
    )
    for extra_effect in (
        _wait_effect(0),
        _dependency_effect(EffectKind.READ, counter_id=_COUNTER_VMEM_LOAD),
        _dependency_effect(EffectKind.WRITE, counter_id=_COUNTER_VMEM_STORE),
        _dependency_effect(EffectKind.BARRIER),
    ):
        with _raises_value_error("must have only concrete counter effects"):
            amdgpu_wait_packet_tables._descriptor_wait_packet_rows(
                replace(descriptor, effects=(*descriptor.effects, extra_effect)),
                {descriptor.key},
                0,
            )


def test_rejects_wait_packets_with_register_or_lease_state() -> None:
    descriptor = _descriptor(
        "amdgpu.s_wait_loadcnt",
        effects=(_wait_effect(_COUNTER_VMEM_LOAD),),
        immediates=(_wait_immediate("loadcnt", _WAIT_COUNTER_VMEM_LOAD_ENCODING_ID),),
    )
    source_lease = StorageLease(
        kind=StorageLeaseKind.SOURCE_READ,
        attachment=StorageLeaseAttachment.OPERAND,
        attachment_index=0,
        unit_offset=0,
        unit_count=1,
        release_scope=StorageLeaseReleaseScope.PROGRESS_CLASS,
        release_class_id=_COUNTER_VMEM_LOAD,
        release_class_name="test.progress",
        release_action_id=1,
        release_action_name="test.release",
        release_reason_id=1,
        release_reason_name="test.retained",
    )
    for invalid_descriptor in (
        replace(descriptor, operands=(Operand("value", OperandRole.OPERAND, ()),)),
        replace(descriptor, operands=(Operand("result", OperandRole.RESULT, ()),)),
        replace(descriptor, storage_leases=(source_lease,)),
    ):
        with _raises_value_error("cannot have register operands or storage leases"):
            amdgpu_wait_packet_tables._descriptor_wait_packet_rows(invalid_descriptor, {descriptor.key}, 0)


def test_rejects_missing_descriptor_ref() -> None:
    descriptor = _descriptor(
        "amdgpu.s_waitcnt",
        effects=(_wait_effect(_COUNTER_VMEM_LOAD),),
        immediates=(_wait_immediate("vmcnt", _WAIT_COUNTER_VMEM_ENCODING_ID),),
    )

    with _raises_value_error("requires a descriptor ref"):
        amdgpu_wait_packet_tables._descriptor_set_wait_packet_rows(
            _descriptor_set(descriptor),
            descriptor_set_ordinal=0,
            descriptor_ref_key_set=set(),
            first_descriptor=0,
            first_immediate=0,
        )


def test_rejects_non_unsigned_immediate() -> None:
    descriptor = _descriptor(
        "amdgpu.s_waitcnt",
        effects=(_wait_effect(_COUNTER_VMEM_LOAD),),
        immediates=(
            _wait_immediate(
                "vmcnt",
                _WAIT_COUNTER_VMEM_ENCODING_ID,
                kind=ImmediateKind.SIGNED,
            ),
        ),
    )

    with _raises_value_error("must be unsigned"):
        amdgpu_wait_packet_tables._descriptor_set_wait_packet_rows(
            _descriptor_set(descriptor),
            descriptor_set_ordinal=0,
            descriptor_ref_key_set={"amdgpu.s_waitcnt"},
            first_descriptor=0,
            first_immediate=0,
        )


def test_rejects_unsupported_immediate_encoding() -> None:
    descriptor = _descriptor(
        "amdgpu.s_waitcnt",
        effects=(_wait_effect(_COUNTER_VMEM_LOAD),),
        immediates=(_wait_immediate("unknowncnt", 0x7FFF),),
    )

    with _raises_value_error("unsupported AMDGPU wait immediate encoding"):
        amdgpu_wait_packet_tables._descriptor_set_wait_packet_rows(
            _descriptor_set(descriptor),
            descriptor_set_ordinal=0,
            descriptor_ref_key_set={"amdgpu.s_waitcnt"},
            first_descriptor=0,
            first_immediate=0,
        )


def test_rejects_no_wait_value_larger_than_uint16() -> None:
    descriptor = _descriptor(
        "amdgpu.s_waitcnt",
        effects=(_wait_effect(_COUNTER_VMEM_LOAD),),
        immediates=(
            _wait_immediate(
                "vmcnt",
                _WAIT_COUNTER_VMEM_ENCODING_ID,
                unsigned_max=0x10000,
            ),
        ),
    )

    with _raises_value_error("expected uint16_t"):
        amdgpu_wait_packet_tables._descriptor_set_wait_packet_rows(
            _descriptor_set(descriptor),
            descriptor_set_ordinal=0,
            descriptor_ref_key_set={"amdgpu.s_waitcnt"},
            first_descriptor=0,
            first_immediate=0,
        )


def test_rejects_no_wait_value_equal_to_full_completion() -> None:
    descriptor = _descriptor(
        "amdgpu.s_waitcnt",
        effects=(_wait_effect(_COUNTER_LDS),),
        immediates=(_wait_immediate("lgkmcnt", _WAIT_COUNTER_LGKM_ENCODING_ID, unsigned_max=0),),
    )
    with _raises_value_error("must distinguish full completion from no wait"):
        amdgpu_wait_packet_tables._descriptor_set_wait_packet_rows(
            _descriptor_set(descriptor),
            descriptor_set_ordinal=0,
            descriptor_ref_key_set={descriptor.key},
            first_descriptor=0,
            first_immediate=0,
        )


def test_rejects_immediate_start_larger_than_uint16() -> None:
    descriptor = _descriptor(
        "amdgpu.s_waitcnt",
        effects=(_wait_effect(_COUNTER_VMEM_LOAD),),
        immediates=(_wait_immediate("vmcnt", _WAIT_COUNTER_VMEM_ENCODING_ID),),
    )

    with _raises_value_error("immediate start"):
        amdgpu_wait_packet_tables._descriptor_set_wait_packet_rows(
            _descriptor_set(descriptor),
            descriptor_set_ordinal=0,
            descriptor_ref_key_set={"amdgpu.s_waitcnt"},
            first_descriptor=0,
            first_immediate=0x10000,
        )


def test_rejects_unmapped_counter_effect() -> None:
    descriptor = _descriptor(
        "amdgpu.s_waitcnt",
        effects=(_wait_effect(_COUNTER_ALU),),
        immediates=(_wait_immediate("vmcnt", _WAIT_COUNTER_VMEM_ENCODING_ID),),
    )

    with _raises_value_error("does not map"):
        amdgpu_wait_packet_tables._descriptor_set_wait_packet_rows(
            _descriptor_set(descriptor),
            descriptor_set_ordinal=0,
            descriptor_ref_key_set={"amdgpu.s_waitcnt"},
            first_descriptor=0,
            first_immediate=0,
        )


def test_combined_immediate_maps_multiple_counter_effects() -> None:
    descriptor = _descriptor(
        "amdgpu.s_waitcnt",
        effects=(_wait_effect(_COUNTER_LDS), _wait_effect(_COUNTER_SMEM)),
        immediates=(_wait_immediate("lgkmcnt", _WAIT_COUNTER_LGKM_ENCODING_ID),),
    )
    descriptor_rows, immediate_rows, _, _ = amdgpu_wait_packet_tables._descriptor_set_wait_packet_rows(
        _descriptor_set(descriptor),
        descriptor_set_ordinal=0,
        descriptor_ref_key_set={"amdgpu.s_waitcnt"},
        first_descriptor=0,
        first_immediate=0,
    )

    assert descriptor_rows[0].counter_count == 2
    assert immediate_rows[0].counter_mask == (amdgpu_wait_packet_tables._counter_mask(_COUNTER_LDS) | amdgpu_wait_packet_tables._counter_mask(_COUNTER_SMEM))

    selection_rows = amdgpu_wait_packet_tables._descriptor_set_wait_packet_selection_rows("amdgpu.test.core", 0, descriptor_rows, immediate_rows)
    combined_mask = immediate_rows[0].counter_mask
    for counter_id in (_COUNTER_LDS, _COUNTER_SMEM):
        counter_mask = amdgpu_wait_packet_tables._counter_mask(counter_id)
        assert selection_rows[counter_mask].covered_counter_mask == counter_mask
        assert selection_rows[counter_mask].full_drain_counter_mask == combined_mask
    assert selection_rows[combined_mask].full_drain_counter_mask == combined_mask
    assert selection_rows[0].full_drain_counter_mask == 0


def test_full_drain_guarantee_survives_alternative_packet_selection() -> None:
    combined = _descriptor(
        "amdgpu.s_waitcnt",
        effects=(_wait_effect(_COUNTER_LDS), _wait_effect(_COUNTER_SMEM)),
        immediates=(_wait_immediate("lgkmcnt", _WAIT_COUNTER_LGKM_ENCODING_ID),),
    )
    scalar = _descriptor(
        "amdgpu.s_wait_kmcnt",
        effects=(_wait_effect(_COUNTER_SMEM),),
        immediates=(_wait_immediate("kmcnt", _WAIT_COUNTER_SMEM_ENCODING_ID),),
    )
    descriptor_rows, immediate_rows, _, _ = amdgpu_wait_packet_tables._descriptor_set_wait_packet_rows(
        _descriptor_set(combined, scalar),
        descriptor_set_ordinal=0,
        descriptor_ref_key_set={combined.key, scalar.key},
        first_descriptor=0,
        first_immediate=0,
    )
    selections = amdgpu_wait_packet_tables._descriptor_set_wait_packet_selection_rows("amdgpu.test.core", 0, descriptor_rows, immediate_rows)
    lds_mask = amdgpu_wait_packet_tables._counter_mask(_COUNTER_LDS)
    scalar_mask = amdgpu_wait_packet_tables._counter_mask(_COUNTER_SMEM)
    # The scalar counter can be drained without touching LDS. The LDS counter
    # has only the combined encoding, which always drains both.
    assert selections[scalar_mask].full_drain_counter_mask == scalar_mask
    assert selections[lds_mask].full_drain_counter_mask == lds_mask | scalar_mask


def test_rejects_full_drain_guarantee_not_implied_by_encoding() -> None:
    descriptor = _descriptor(
        "amdgpu.s_wait_kmcnt",
        effects=(_wait_effect(_COUNTER_SMEM),),
        immediates=(_wait_immediate("kmcnt", _WAIT_COUNTER_SMEM_ENCODING_ID),),
    )
    descriptors, immediates, lookups, descriptor_range = amdgpu_wait_packet_tables._descriptor_set_wait_packet_rows(
        _descriptor_set(descriptor),
        descriptor_set_ordinal=0,
        descriptor_ref_key_set={descriptor.key},
        first_descriptor=0,
        first_immediate=0,
    )
    selections = amdgpu_wait_packet_tables._descriptor_set_wait_packet_selection_rows("amdgpu.test.core", 0, descriptors, immediates)
    tables = amdgpu_wait_packet_tables._WaitPacketTables(
        descriptor_rows=descriptors,
        immediate_rows=immediates,
        range_rows=(descriptor_range,),
        descriptor_lookup_rows=lookups,
        selection_rows=selections,
    )
    amdgpu_wait_packet_tables._validate_wait_packet_tables(tables)
    scalar_mask = amdgpu_wait_packet_tables._counter_mask(_COUNTER_SMEM)
    lds_mask = amdgpu_wait_packet_tables._counter_mask(_COUNTER_LDS)
    invalid_selections = tuple(replace(row, full_drain_counter_mask=scalar_mask | lds_mask) if row.counter_mask == scalar_mask else row for row in selections)
    with _raises_value_error("full drain mask does not match encoding guarantees"):
        amdgpu_wait_packet_tables._validate_wait_packet_tables(replace(tables, selection_rows=invalid_selections))
    invalid_selections = (replace(selections[0], full_drain_counter_mask=scalar_mask), *selections[1:])
    with _raises_value_error("claims completion without a covered counter"):
        amdgpu_wait_packet_tables._validate_wait_packet_tables(replace(tables, selection_rows=invalid_selections))


def test_validates_required_counter_packet_coverage() -> None:
    wait_descriptor = _descriptor(
        "amdgpu.s_waitcnt",
        effects=(_wait_effect(_COUNTER_VMEM_LOAD),),
        immediates=(_wait_immediate("vmcnt", _WAIT_COUNTER_VMEM_ENCODING_ID),),
    )
    producer_descriptor = _descriptor(
        "amdgpu.global_load",
        effects=(_dependency_effect(EffectKind.READ),),
        immediates=(),
    )
    descriptor_set = _descriptor_set(
        wait_descriptor,
        producer_descriptor,
        hazards=(Hazard(HazardKind.WAIT_COUNTER, counter_id=_COUNTER_VMEM_LOAD),),
    )
    descriptor_rows, immediate_rows, _, _ = amdgpu_wait_packet_tables._descriptor_set_wait_packet_rows(
        descriptor_set,
        descriptor_set_ordinal=0,
        descriptor_ref_key_set={"amdgpu.s_waitcnt"},
        first_descriptor=0,
        first_immediate=0,
    )
    selection_rows = amdgpu_wait_packet_tables._descriptor_set_wait_packet_selection_rows(descriptor_set.key, 0, descriptor_rows, immediate_rows)

    amdgpu_wait_packet_tables._validate_descriptor_set_wait_packet_coverage(descriptor_set, descriptor_rows, selection_rows)


def test_default_write_effect_uses_smem_wait_counter() -> None:
    wait_descriptor = _descriptor(
        "amdgpu.s_waitcnt",
        effects=(_wait_effect(_COUNTER_SMEM),),
        immediates=(_wait_immediate("lgkmcnt", _WAIT_COUNTER_SMEM_ENCODING_ID),),
    )
    producer_descriptor = _descriptor(
        "amdgpu.s_store_dword",
        effects=(_dependency_effect(EffectKind.WRITE),),
        immediates=(),
    )
    descriptor_set = _descriptor_set(
        wait_descriptor,
        producer_descriptor,
        hazards=(Hazard(HazardKind.WAIT_COUNTER, counter_id=_COUNTER_SMEM),),
    )
    descriptor_rows, immediate_rows, _, _ = amdgpu_wait_packet_tables._descriptor_set_wait_packet_rows(
        descriptor_set,
        descriptor_set_ordinal=0,
        descriptor_ref_key_set={"amdgpu.s_waitcnt"},
        first_descriptor=0,
        first_immediate=0,
    )
    selection_rows = amdgpu_wait_packet_tables._descriptor_set_wait_packet_selection_rows(descriptor_set.key, 0, descriptor_rows, immediate_rows)

    amdgpu_wait_packet_tables._validate_descriptor_set_wait_packet_coverage(descriptor_set, descriptor_rows, selection_rows)


def test_rejects_missing_required_counter_packet_coverage() -> None:
    producer = _descriptor(
        "amdgpu.async_producer",
        effects=(_dependency_effect(EffectKind.READ),),
        immediates=(),
    )
    descriptor_set = _descriptor_set(
        producer,
        hazards=(Hazard(HazardKind.WAIT_COUNTER, counter_id=_COUNTER_LDS),),
    )
    selection_rows = amdgpu_wait_packet_tables._descriptor_set_wait_packet_selection_rows(descriptor_set.key, 0, (), ())

    with _raises_value_error("packet descriptors only cover"):
        amdgpu_wait_packet_tables._validate_descriptor_set_wait_packet_coverage(descriptor_set, (), selection_rows)


def test_rejects_dependency_counter_missing_from_schedule_hazards() -> None:
    producer = _descriptor(
        "amdgpu.async_producer",
        effects=(_dependency_effect(EffectKind.READ, counter_id=_COUNTER_SMEM),),
        immediates=(),
    )
    descriptor_set = _descriptor_set(
        producer,
        hazards=(Hazard(HazardKind.WAIT_COUNTER, counter_id=_COUNTER_VMEM_LOAD),),
    )
    selection_rows = amdgpu_wait_packet_tables._descriptor_set_wait_packet_selection_rows(descriptor_set.key, 0, (), ())

    with _raises_value_error("is not present in schedule class"):
        amdgpu_wait_packet_tables._validate_descriptor_set_wait_packet_coverage(descriptor_set, (), selection_rows)


def test_requires_alu_wait_packet_for_depctr_processors() -> None:
    descriptor_set = _descriptor_set()
    selection_rows = amdgpu_wait_packet_tables._descriptor_set_wait_packet_selection_rows(descriptor_set.key, 0, (), ())

    with _raises_value_error("packet descriptors only cover"):
        amdgpu_wait_packet_tables._validate_descriptor_set_wait_packet_coverage(
            descriptor_set,
            (),
            selection_rows,
            processor_scheduling_bits=(AMDGPU_PROCESSOR_SCHEDULING_VALU_TRANS_USE_DEPCTR),
        )


def test_rejects_descriptor_row_with_out_of_bounds_immediates() -> None:
    tables = amdgpu_wait_packet_tables._WaitPacketTables(
        descriptor_rows=(
            amdgpu_wait_packet_tables._WaitPacketDescriptorRow(
                descriptor_set_key="amdgpu.test.core",
                descriptor_set_ordinal=0,
                descriptor_key="amdgpu.s_waitcnt",
                descriptor_ref="LOOM_AMDGPU_DESCRIPTOR_REF_S_WAITCNT",
                counter_mask=amdgpu_wait_packet_tables._counter_mask(_COUNTER_VMEM_LOAD),
                counter_count=1,
                immediate_start=1,
                immediate_count=1,
            ),
        ),
        immediate_rows=(),
        range_rows=(),
        descriptor_lookup_rows=(),
        selection_rows=(),
    )

    with _raises_value_error("immediate range is out of bounds"):
        amdgpu_wait_packet_tables._validate_wait_packet_tables(tables)


def test_rejects_descriptor_range_with_out_of_bounds_descriptors() -> None:
    tables = amdgpu_wait_packet_tables._WaitPacketTables(
        descriptor_rows=(),
        immediate_rows=(),
        range_rows=(
            amdgpu_wait_packet_tables._WaitPacketDescriptorRange(
                descriptor_set_key="amdgpu.test.core",
                descriptor_set_ordinal=0,
                first_descriptor=1,
                descriptor_count=1,
                first_descriptor_lookup=0,
                descriptor_lookup_count=0,
                max_descriptor_immediate_count=1,
                maximum_target_counts=(62, 0, 0, 0, 0, 0, 0, 0),
            ),
        ),
        descriptor_lookup_rows=(),
        selection_rows=(),
    )

    with _raises_value_error("descriptor range is out of bounds"):
        amdgpu_wait_packet_tables._validate_wait_packet_tables(tables)


def test_rejects_selection_row_with_out_of_bounds_descriptor() -> None:
    tables = amdgpu_wait_packet_tables._WaitPacketTables(
        descriptor_rows=(
            amdgpu_wait_packet_tables._WaitPacketDescriptorRow(
                descriptor_set_key="amdgpu.test.core",
                descriptor_set_ordinal=0,
                descriptor_key="amdgpu.s_waitcnt",
                descriptor_ref="LOOM_AMDGPU_DESCRIPTOR_REF_S_WAITCNT",
                counter_mask=amdgpu_wait_packet_tables._counter_mask(_COUNTER_VMEM_LOAD),
                counter_count=1,
                immediate_start=0,
                immediate_count=1,
            ),
        ),
        immediate_rows=(
            amdgpu_wait_packet_tables._WaitPacketImmediateRow(
                descriptor_key="amdgpu.s_waitcnt",
                descriptor_immediate_index=0,
                field_name="vmcnt",
                counter_mask=amdgpu_wait_packet_tables._counter_mask(_COUNTER_VMEM_LOAD),
                no_wait_value=63,
            ),
        ),
        range_rows=(
            amdgpu_wait_packet_tables._WaitPacketDescriptorRange(
                descriptor_set_key="amdgpu.test.core",
                descriptor_set_ordinal=0,
                first_descriptor=0,
                descriptor_count=1,
                first_descriptor_lookup=0,
                descriptor_lookup_count=1,
                max_descriptor_immediate_count=1,
                maximum_target_counts=(62, 0, 0, 0, 0, 0, 0, 0),
            ),
        ),
        descriptor_lookup_rows=(1,),
        selection_rows=tuple(
            amdgpu_wait_packet_tables._WaitPacketSelectionRow(
                descriptor_set_key="amdgpu.test.core",
                descriptor_set_ordinal=0,
                counter_mask=counter_mask,
                descriptor_index=2 if counter_mask == amdgpu_wait_packet_tables._counter_mask(_COUNTER_VMEM_LOAD) else 0,
                covered_counter_mask=amdgpu_wait_packet_tables._counter_mask(_COUNTER_VMEM_LOAD) if counter_mask == amdgpu_wait_packet_tables._counter_mask(_COUNTER_VMEM_LOAD) else 0,
                full_drain_counter_mask=0,
            )
            for counter_mask in range(amdgpu_wait_packet_tables._WAIT_COUNTER_MASK_COUNT)
        ),
    )

    with _raises_value_error("descriptor index is out of bounds"):
        amdgpu_wait_packet_tables._validate_wait_packet_tables(tables)


def test_rejects_descriptor_lookup_row_with_out_of_bounds_descriptor() -> None:
    tables = amdgpu_wait_packet_tables._WaitPacketTables(
        descriptor_rows=(
            amdgpu_wait_packet_tables._WaitPacketDescriptorRow(
                descriptor_set_key="amdgpu.test.core",
                descriptor_set_ordinal=0,
                descriptor_key="amdgpu.s_waitcnt",
                descriptor_ref="LOOM_AMDGPU_DESCRIPTOR_REF_S_WAITCNT",
                counter_mask=amdgpu_wait_packet_tables._counter_mask(_COUNTER_VMEM_LOAD),
                counter_count=1,
                immediate_start=0,
                immediate_count=1,
            ),
        ),
        immediate_rows=(
            amdgpu_wait_packet_tables._WaitPacketImmediateRow(
                descriptor_key="amdgpu.s_waitcnt",
                descriptor_immediate_index=0,
                field_name="vmcnt",
                counter_mask=amdgpu_wait_packet_tables._counter_mask(_COUNTER_VMEM_LOAD),
                no_wait_value=63,
            ),
        ),
        range_rows=(
            amdgpu_wait_packet_tables._WaitPacketDescriptorRange(
                descriptor_set_key="amdgpu.test.core",
                descriptor_set_ordinal=0,
                first_descriptor=0,
                descriptor_count=1,
                first_descriptor_lookup=0,
                descriptor_lookup_count=1,
                max_descriptor_immediate_count=1,
                maximum_target_counts=(62, 0, 0, 0, 0, 0, 0, 0),
            ),
        ),
        descriptor_lookup_rows=(2,),
        selection_rows=tuple(
            amdgpu_wait_packet_tables._WaitPacketSelectionRow(
                descriptor_set_key="amdgpu.test.core",
                descriptor_set_ordinal=0,
                counter_mask=counter_mask,
                descriptor_index=0,
                covered_counter_mask=0,
                full_drain_counter_mask=0,
            )
            for counter_mask in range(amdgpu_wait_packet_tables._WAIT_COUNTER_MASK_COUNT)
        ),
    )

    with _raises_value_error("descriptor index is out of bounds"):
        amdgpu_wait_packet_tables._validate_wait_packet_tables(tables)
