# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Public diagnostics for unsupported C++ semantic projections."""

from loom.ir import Operation


class CxxRenderError(RuntimeError):
    """A precise semantic boundary that has no valid C++ projection."""

    def __init__(self, operation: Operation | None, reason: str):
        self.operation = operation
        self.reason = reason
        name = operation.name if operation is not None else "module"
        super().__init__(f"cannot project {name}: {reason}")
