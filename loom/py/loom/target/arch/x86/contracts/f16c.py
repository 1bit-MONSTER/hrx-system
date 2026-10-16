# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Optional F16C source conversion contract fragment."""

from loom.dialect.scalar import ALL_SCALAR_OPS
from loom.dialect.vector import ALL_VECTOR_OPS
from loom.target.arch.x86.contracts.half_conversion import half_conversion_rules
from loom.target.arch.x86.descriptors import X86_AVX2_FEATURES_DESCRIPTOR_SET
from loom.target.contracts import ContractFragment, descriptor_by_key

X86_F16C_CONTRACT_DIALECT_OPS = {
    "scalar": ALL_SCALAR_OPS,
    "vector": ALL_VECTOR_OPS,
}

X86_F16C_CONTRACT_FRAGMENT = ContractFragment(
    name="x86.f16c",
    descriptor_set=X86_AVX2_FEATURES_DESCRIPTOR_SET,
    public_header="loom/target/arch/x86/contracts/f16c.h",
    cases=half_conversion_rules(
        lambda key: descriptor_by_key(X86_AVX2_FEATURES_DESCRIPTOR_SET, key),
        evex=False,
    ),
)
