# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Source closure and variants for the embedding serving fixture."""

SOURCE_FILES = [
    "config.loom",
    "control.loom",
    "embed.loom",
    "model.json",
    "prepare.loom",
    "tokenizer.json",
]

VARIANT_FILES = [
    "variants/direct.loom",
    "variants/policy.loom",
    "variants/weights.loom",
]

SOURCES = ["//loom/src/loom/serve/fixture/embedding:" + path for path in SOURCE_FILES]
VARIANT_SOURCES = ["//loom/src/loom/serve/fixture/embedding:" + path for path in VARIANT_FILES]
