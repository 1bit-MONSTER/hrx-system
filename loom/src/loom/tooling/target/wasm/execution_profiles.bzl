# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Execution profiles owned by the hosted WebAssembly target provider."""

load("//loom/build_tools/bazel:defs.bzl", "loom_execution_profile")
load(
    "//loom/requirements:defs.bzl",
    "EMIT_WASM",
    "TARGET_ARCH_VM",
    "TARGET_ARCH_WASM",
)

WASM_HOSTED_PROFILE = loom_execution_profile(
    name = "wasm_hosted",
    build_requirements = [
        TARGET_ARCH_VM,
        TARGET_ARCH_WASM,
        EMIT_WASM,
    ],
    executor = "hosted",
    runner = "//loom/src/loom/tools/iree-test-loom:iree-test-loom-wasi",
    target_class = "cpu",
    target_family = "wasm",
)
