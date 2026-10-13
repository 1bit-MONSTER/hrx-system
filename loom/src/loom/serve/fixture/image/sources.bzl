# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Source closure for the image serving fixture."""

SOURCE_FILES = [
    "config.loom",
    "control.loom",
    "generate.loom",
    "model.json",
    "prepare.loom",
    "tokenizer.json",
]

SOURCES = ["//loom/src/loom/serve/fixture/image:" + path for path in SOURCE_FILES]
