# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Node.js execution policy for ordinary Wasm module exports."""

load("//loom/build_tools/bazel:defs.bzl", "loom_execution_profile")
load(
    "//loom/requirements:defs.bzl",
    "EMIT_WASM",
    "TARGET_ARCH_WASM",
)

WASM_NODE_PROFILE = loom_execution_profile(
    name = "wasm_node",
    build_requirements = [
        TARGET_ARCH_WASM,
        EMIT_WASM,
    ],
    env_inherit = [
        "IREE_WASM_NODE",
        "PATH",
    ],
    executor = "node",
    runner_args = ["--target=wasm:simd128"],
    tags = ["hostonly"],
    target_class = "cpu",
    target_family = "wasm",
)
