# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""AMDGPU register dependency and execution resource timing."""

from __future__ import annotations

from dataclasses import replace

from loom.target.low_descriptors import (
    Descriptor,
    DescriptorSet,
    EffectKind,
    EventSeparation,
    IssueUse,
    MemorySpace,
    ModelQuality,
    Operand,
    OperandFlag,
    OperandRole,
    Resource,
    ResourceKind,
    ScheduleClass,
    TimingEvent,
)

from .common import (
    _REG_SGPR,
    _SCHEDULE_LDS_ATOMIC,
    _SCHEDULE_LDS_CROSSLANE,
    _SCHEDULE_LDS_LOAD,
    _SCHEDULE_LDS_STORE,
    _SCHEDULE_VALU,
)


def _with_lds_service_timing(
    descriptor_set: DescriptorSet, bits_per_lane_per_cycle: int
) -> DescriptorSet:
    # Independent addresses still contend for the LDS execution pipeline.
    # Model the minimum service of one SIMD issue pass from packet width;
    # bank conflicts, wave64 replay, and other waves can increase this cost.
    # This bandwidth reservation is distinct from result availability and
    # register-source leases. Keep their existing latency/hazard models.
    service_resource = "amdgpu.lds.service"
    parents = {
        row.name: row
        for row in descriptor_set.schedule_classes
        if row.name
        in (
            _SCHEDULE_LDS_LOAD,
            _SCHEDULE_LDS_STORE,
            _SCHEDULE_LDS_ATOMIC,
            _SCHEDULE_LDS_CROSSLANE,
        )
    }
    variants: dict[tuple[str, int], ScheduleClass] = {}
    descriptors: list[Descriptor] = []
    for descriptor in descriptor_set.descriptors:
        parent = parents.get(descriptor.schedule_class)
        if parent is None:
            descriptors.append(descriptor)
            continue
        # An atomic's read and write effects describe the same packet width.
        # Cross-lane packets use the LDS pipeline without a memory footprint.
        width_bits = max(
            (
                effect.width_bits
                for effect in descriptor.effects
                if effect.memory_space == MemorySpace.WORKGROUP
                and effect.kind in (EffectKind.READ, EffectKind.WRITE)
            ),
            default=0,
        )
        cycles = max(
            1,
            (width_bits + bits_per_lane_per_cycle - 1) // bits_per_lane_per_cycle,
        )
        key = (parent.name, cycles)
        if key not in variants:
            variants[key] = replace(
                parent,
                name=f"{parent.name}.service{cycles}",
                issue_uses=(
                    *parent.issue_uses,
                    IssueUse(service_resource, cycles=cycles, units=1),
                ),
            )
        descriptors.append(replace(descriptor, schedule_class=variants[key].name))
    return replace(
        descriptor_set,
        descriptors=tuple(descriptors),
        resources=(
            *descriptor_set.resources,
            Resource(
                service_resource, capacity_per_cycle=1, kind=ResourceKind.PIPELINE
            ),
        ),
        schedule_classes=(*descriptor_set.schedule_classes, *variants.values()),
    )


def _with_valu_sgpr_timing(
    descriptor_set: DescriptorSet, separation_cycles: int
) -> DescriptorSet:
    # Compare masks and lane reads are produced by VALU into scalar registers.
    # Model their availability at operand endpoints so independent instructions
    # can fill the dependency gap. Class latency also controls completion and
    # critical-path priority, which have different scheduling consequences.
    read_event = "amdgpu.sgpr.read"
    write_event = "amdgpu.valu.sgpr.write"
    descriptors: list[Descriptor] = []
    for descriptor in descriptor_set.descriptors:
        operands: list[Operand] = []
        for operand in descriptor.operands:
            has_sgpr = any(
                alternative.reg_class == _REG_SGPR for alternative in operand.reg_alts
            )
            reads_sgpr = has_sgpr and (
                operand.role
                in (
                    OperandRole.OPERAND,
                    OperandRole.OPERAND_RESULT,
                    OperandRole.PREDICATE,
                    OperandRole.RESOURCE,
                )
                or OperandFlag.STATE_READ in operand.flags
            )
            writes_sgpr = (
                has_sgpr
                and descriptor.schedule_class == _SCHEDULE_VALU
                and (
                    operand.role in (OperandRole.RESULT, OperandRole.OPERAND_RESULT)
                    or OperandFlag.STATE_WRITE in operand.flags
                )
            )
            if writes_sgpr and any(
                alternative.reg_class != _REG_SGPR for alternative in operand.reg_alts
            ):
                raise ValueError(
                    f"AMDGPU descriptor '{descriptor.key}' result "
                    f"'{operand.field_name}' requires distinct SGPR and VGPR forms "
                    "to model VALU write timing"
                )
            operands.append(
                replace(
                    operand,
                    read_event=read_event if reads_sgpr else operand.read_event,
                    write_event=write_event if writes_sgpr else operand.write_event,
                )
            )
        descriptors.append(replace(descriptor, operands=tuple(operands)))
    return replace(
        descriptor_set,
        descriptors=tuple(descriptors),
        timing_events=(
            *descriptor_set.timing_events,
            TimingEvent(read_event),
            TimingEvent(write_event),
        ),
        event_separations=(
            *descriptor_set.event_separations,
            EventSeparation(
                write_event, read_event, separation_cycles, ModelQuality.ESTIMATED
            ),
        ),
    )
