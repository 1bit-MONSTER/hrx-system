# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Source-of-truth rows for SPIR-V shader built-in coordinate loads."""

from __future__ import annotations

from dataclasses import dataclass

from loom.target.arch.spirv.features import feature_bits_value


@dataclass(frozen=True, slots=True)
class BuiltinDimension:
    source_keyword: str
    component_index: int


@dataclass(frozen=True, slots=True)
class BuiltinQuery:
    source_op_key: str
    descriptor_suffix: str
    mnemonic_suffix: str
    builtin_enum: str
    component_count: int
    feature_atoms: tuple[str, ...] = ()

    @property
    def feature_bits(self) -> int:
        return feature_bits_value(self.feature_atoms)

    @property
    def variable_enum(self) -> str:
        return f"LOOM_SPIRV_BUILTIN_VARIABLE_{self.source_op_key.upper()}"


BUILTIN_DIMENSIONS = (
    BuiltinDimension("x", 0),
    BuiltinDimension("y", 1),
    BuiltinDimension("z", 2),
)

BUILTIN_INDEX_QUERIES = (
    BuiltinQuery(
        source_op_key="workgroup_id",
        descriptor_suffix="workgroup_id",
        mnemonic_suffix="workgroup_id",
        builtin_enum="LOOM_SPIRV_BUILT_IN_WORKGROUP_ID",
        component_count=3,
    ),
    BuiltinQuery(
        source_op_key="workitem_id",
        descriptor_suffix="workitem_id",
        mnemonic_suffix="local_invocation_id",
        builtin_enum="LOOM_SPIRV_BUILT_IN_LOCAL_INVOCATION_ID",
        component_count=3,
    ),
    BuiltinQuery(
        source_op_key="workitem_dispatch_id",
        descriptor_suffix="workitem_dispatch_id",
        mnemonic_suffix="global_invocation_id",
        builtin_enum="LOOM_SPIRV_BUILT_IN_GLOBAL_INVOCATION_ID",
        component_count=3,
    ),
    BuiltinQuery(
        source_op_key="workgroup_count",
        descriptor_suffix="workgroup_count",
        mnemonic_suffix="num_workgroups",
        builtin_enum="LOOM_SPIRV_BUILT_IN_NUM_WORKGROUPS",
        component_count=3,
    ),
)

BUILTIN_SCALAR_INDEX_QUERIES = (
    BuiltinQuery(
        source_op_key="subgroup_lane_id",
        descriptor_suffix="subgroup_lane_id",
        mnemonic_suffix="subgroup_local_invocation_id",
        builtin_enum="LOOM_SPIRV_BUILT_IN_SUBGROUP_LOCAL_INVOCATION_ID",
        component_count=1,
        feature_atoms=("group_non_uniform",),
    ),
    BuiltinQuery(
        source_op_key="subgroup_size",
        descriptor_suffix="subgroup_size",
        mnemonic_suffix="subgroup_size",
        builtin_enum="LOOM_SPIRV_BUILT_IN_SUBGROUP_SIZE",
        component_count=1,
        feature_atoms=("group_non_uniform",),
    ),
    BuiltinQuery(
        source_op_key="subgroup_id",
        descriptor_suffix="subgroup_id",
        mnemonic_suffix="subgroup_id",
        builtin_enum="LOOM_SPIRV_BUILT_IN_SUBGROUP_ID",
        component_count=1,
        feature_atoms=("group_non_uniform",),
    ),
    BuiltinQuery(
        source_op_key="subgroup_count",
        descriptor_suffix="subgroup_count",
        mnemonic_suffix="num_subgroups",
        builtin_enum="LOOM_SPIRV_BUILT_IN_NUM_SUBGROUPS",
        component_count=1,
        feature_atoms=("group_non_uniform",),
    ),
)
