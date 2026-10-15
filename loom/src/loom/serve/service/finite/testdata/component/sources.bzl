# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Source closure for the weighted finite-component fixture."""

SOURCE_FILES = [
    "testdata/component/control.loom",
    "testdata/component/kernels.loom",
    "testdata/component/policy.loom",
    "testdata/component/prepare.loom",
]

SOURCES = ["//loom/src/loom/serve/service/finite:" + path for path in SOURCE_FILES]
