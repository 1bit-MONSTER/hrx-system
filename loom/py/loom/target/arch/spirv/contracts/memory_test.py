# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

from loom.target.arch.spirv.contracts.logical_core import (
    SPIRV_LOGICAL_CORE_CONTRACT_FRAGMENT,
)
from loom.target.contracts import DescriptorRule, SourceMemoryOperation


def test_constant_storage_is_readable_across_storage_memory_contracts() -> None:
    operations = set()
    for rule in SPIRV_LOGICAL_CORE_CONTRACT_FRAGMENT.cases:
        if not isinstance(rule, DescriptorRule):
            continue
        for step in rule.emit:
            memory = step.source_memory
            if memory is None or "global" not in memory.memory_spaces:
                continue
            operations.add(memory.operation)
            assert ("constant" in memory.memory_spaces) == (
                memory.operation is SourceMemoryOperation.LOAD
            ), (rule.source_op.name, step.descriptor.key)
    assert operations == {
        SourceMemoryOperation.LOAD,
        SourceMemoryOperation.STORE,
        SourceMemoryOperation.ATOMIC_REDUCE,
        SourceMemoryOperation.ATOMIC_RMW,
        SourceMemoryOperation.ATOMIC_CMPXCHG,
    }
