# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Authored source inventory for function semantic conformance."""

load("//loom/build_tools/bazel:defs.bzl", "loom_corpus_manifest")

FUNCTION_CORPUS = loom_corpus_manifest(
    name = "function",
    package = "//loom/src/loom/test/corpus/function",
    srcs = [
        "buffer_arguments.loom",
        "buffer_call_overflow.loom",
        "buffer_calls.loom",
        "call_overflow.loom",
        "calls.loom",
        "direct_call.loom",
        "rodata.loom",
        "spill.loom",
        "template_expansion.loom",
        "template_library.loom",
        "template_motion.loom",
        "template_relations.loom",
    ],
)
