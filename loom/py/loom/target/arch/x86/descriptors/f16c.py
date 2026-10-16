# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Optional F16C conversion descriptors for AVX2 CPU profiles."""

from pathlib import Path

from loom.target.low_descriptors import DescriptorSet

from .avx2 import X86_AVX2_DESCRIPTOR_SET
from .common import _descriptor_support_tables
from .half_conversion import half_conversion_descriptors

_DESCRIPTORS = half_conversion_descriptors(evex=False)
_REGISTER_CLASSES, _RESOURCES, _SCHEDULE_CLASSES = _descriptor_support_tables(
    _DESCRIPTORS, X86_AVX2_DESCRIPTOR_SET
)

X86_F16C_DESCRIPTOR_SET = DescriptorSet(
    key="x86.f16c.core",
    target_key="x86",
    feature_key="x86.f16c.v1",
    c_header_path=Path("loom/src/loom/target/arch/x86/descriptors/f16c_descriptors.h"),
    c_source_path=Path("loom/src/loom/target/arch/x86/f16c_descriptors.c"),
    header_guard="LOOM_TARGET_ARCH_X86_F16C_DESCRIPTORS_H_",
    public_header="loom/target/arch/x86/descriptors/f16c_descriptors.h",
    function_name="loom_x86_f16c_core_descriptor_set",
    c_table_prefix="X86F16cCore",
    c_enum_prefix="X86_F16C_CORE",
    generator_version=1,
    supports_native_scheduling=True,
    reg_classes=_REGISTER_CLASSES,
    resources=_RESOURCES,
    schedule_classes=_SCHEDULE_CLASSES,
    descriptors=_DESCRIPTORS,
)
