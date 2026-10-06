# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

from __future__ import annotations

from loom.target.arch.amd.xdna.aie2p.array_descriptors import (
    AIE2P_ARRAY_DESCRIPTOR_SET,
)
from loom.target.low_descriptors import (
    Constraint,
    ConstraintKind,
    DescriptorFlag,
    DescriptorOpKind,
    ImmediateFlag,
    ImmediateKind,
    RegClassFlag,
)


def test_array_descriptor_set_is_a_resident_topology_contract() -> None:
    descriptor_set = AIE2P_ARRAY_DESCRIPTOR_SET

    assert descriptor_set.key == "amd.xdna.aie2p.array"
    assert descriptor_set.feature_key == "amd.xdna.aie2p.array.v3"
    assert not descriptor_set.physical_registers
    assert not descriptor_set.physical_register_views
    assert not descriptor_set.register_parts
    assert not descriptor_set.timing_events
    assert not descriptor_set.event_separations
    assert all(
        RegClassFlag.VIRTUAL_ONLY in reg_class.flags
        and RegClassFlag.REFERENCE in reg_class.flags
        and RegClassFlag.UNSPILLABLE in reg_class.flags
        for reg_class in descriptor_set.reg_classes
    )


def test_topology_parameters_are_ssa_operands() -> None:
    descriptors = {
        descriptor.key: descriptor
        for descriptor in AIE2P_ARRAY_DESCRIPTOR_SET.descriptors
    }

    constant = descriptors["amd.xdna.aie2p.array.constant.u32"]
    assert constant.op_kind is DescriptorOpKind.CONST
    assert constant.immediates[0].field_name == "value"
    offset = descriptors["amd.xdna.aie2p.array.constant.u64"]
    assert offset.op_kind is DescriptorOpKind.CONST
    assert offset.immediates[0].bit_width == 64

    group = descriptors["amd.xdna.aie2p.array.group"]
    assert [operand.field_name for operand in group.operands] == ["result", "lanes"]
    assert not group.immediates
    assert DescriptorFlag.DEAD_REMOVABLE in group.flags
    assert DescriptorFlag.UNIQUE_IDENTITY in group.flags

    channel = descriptors["amd.xdna.aie2p.array.channel"]
    assert [operand.field_name for operand in channel.operands] == [
        "result",
        "sender",
        "receiver",
        "capacity",
        "records",
    ]
    assert not channel.immediates

    location = descriptors["amd.xdna.aie2p.array.constrain.location"]
    assert [operand.field_name for operand in location.operands] == [
        "worker",
        "column",
        "row",
    ]
    assert not location.immediates


def test_worker_entries_are_explicit_symbolic_product_edges() -> None:
    workers = [
        descriptor
        for descriptor in AIE2P_ARRAY_DESCRIPTOR_SET.descriptors
        if descriptor.key
        in {"amd.xdna.aie2p.array.worker", "amd.xdna.aie2p.array.worker.fold"}
    ]

    assert [worker.asm_forms[0].mnemonic for worker in workers] == [
        "worker",
        "worker.fold",
    ]
    for worker in workers:
        entry = next(
            immediate
            for immediate in worker.immediates
            if immediate.field_name == "entry"
        )
        assert entry.kind is ImmediateKind.ORDINAL
        assert entry.flags == (ImmediateFlag.SYMBOLIC,)
        assert DescriptorFlag.SIDE_EFFECTING in worker.flags

    fold = workers[1]
    assert [operand.field_name for operand in fold.operands] == [
        "result",
        "group",
        "lane",
        "records",
    ]
    assert [immediate.field_name for immediate in fold.immediates] == [
        "entry",
        "output_port",
        "output_count",
        "kind",
        "fast_math",
    ]
    assert fold.immediates[3].kind is ImmediateKind.ENUM


def test_planner_immediate_fields_are_required_and_typed() -> None:
    # Generated bindings own field positions; the planner consumes these value
    # domains unconditionally after verification.
    fields = {
        "binding": (
            ("access", ImmediateKind.ENUM),
            ("ordinal", ImmediateKind.UNSIGNED),
        ),
        "worker": (("entry", ImmediateKind.ORDINAL),),
        "worker.fold": (
            ("entry", ImmediateKind.ORDINAL),
            ("fast_math", ImmediateKind.UNSIGNED),
            ("kind", ImmediateKind.ENUM),
            ("output_count", ImmediateKind.UNSIGNED),
            ("output_port", ImmediateKind.UNSIGNED),
        ),
        "sender": (("port", ImmediateKind.UNSIGNED),),
        "receiver": (("port", ImmediateKind.UNSIGNED),),
    }
    descriptors = {
        descriptor.mnemonic: descriptor
        for descriptor in AIE2P_ARRAY_DESCRIPTOR_SET.descriptors
    }
    for mnemonic, required_fields in fields.items():
        descriptor = descriptors[mnemonic]
        assert descriptor.op_kind is DescriptorOpKind.OP
        immediates = {value.field_name: value for value in descriptor.immediates}
        for field_name, kind in required_fields:
            immediate = immediates[field_name]
            assert immediate.kind is kind
            assert immediate.flags == (
                (ImmediateFlag.SYMBOLIC,) if immediate.field_name == "entry" else ()
            )


def test_channels_are_typed_persistent_topology_edges() -> None:
    descriptors = {
        descriptor.key: descriptor
        for descriptor in AIE2P_ARRAY_DESCRIPTOR_SET.descriptors
    }
    sender = descriptors["amd.xdna.aie2p.array.sender"]
    receiver = descriptors["amd.xdna.aie2p.array.receiver"]
    sender_partition = descriptors["amd.xdna.aie2p.array.partition.sender"]
    receiver_partition = descriptors["amd.xdna.aie2p.array.partition.receiver"]
    sender_view = descriptors["amd.xdna.aie2p.array.view.sender"]
    receiver_view = descriptors["amd.xdna.aie2p.array.view.receiver"]
    channel = descriptors["amd.xdna.aie2p.array.channel"]

    assert {alternative.reg_class for alternative in sender.operands[1].reg_alts} == {
        "aie2p.array.binding",
        "aie2p.array.worker",
    }
    assert {alternative.reg_class for alternative in receiver.operands[1].reg_alts} == {
        "aie2p.array.binding",
        "aie2p.array.worker",
    }
    for partition, register_class in (
        (sender_partition, "aie2p.array.sender"),
        (receiver_partition, "aie2p.array.receiver"),
    ):
        assert [operand.field_name for operand in partition.operands] == [
            "result",
            "source",
            "offset",
            "lane",
            "lanes",
        ]
        assert all(
            alternative.reg_class == register_class
            for operand in partition.operands[:2]
            for alternative in operand.reg_alts
        )
        assert all(
            alternative.reg_class == "aie2p.array.offset"
            for alternative in partition.operands[2].reg_alts
        )
    for view, register_class in (
        (sender_view, "aie2p.array.sender"),
        (receiver_view, "aie2p.array.receiver"),
    ):
        assert [operand.field_name for operand in view.operands] == [
            "result",
            "source",
            "offset",
        ]
        assert all(
            alternative.reg_class == register_class
            for operand in view.operands[:2]
            for alternative in operand.reg_alts
        )
        assert all(
            alternative.reg_class == "aie2p.array.offset"
            for alternative in view.operands[2].reg_alts
        )
    assert DescriptorFlag.SIDE_EFFECTING in channel.flags
    assert channel.constraints == (
        Constraint(ConstraintKind.SAME_REGISTER_VALUE_TYPE, 0, 1),
        Constraint(ConstraintKind.SAME_REGISTER_VALUE_TYPE, 0, 2),
    )


def test_asm_mnemonics_are_target_relative() -> None:
    mnemonics = [
        descriptor.asm_forms[0].mnemonic
        for descriptor in AIE2P_ARRAY_DESCRIPTOR_SET.descriptors
    ]
    assert len(mnemonics) == len(set(mnemonics))
    assert set(mnemonics) == {
        "binding",
        "channel",
        "constant.u64",
        "constant.u32",
        "constrain.location",
        "group",
        "partition.receiver",
        "partition.sender",
        "receiver",
        "sender",
        "view.receiver",
        "view.sender",
        "worker",
        "worker.fold",
    }
