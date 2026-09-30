# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Authored source inventory for control semantic conformance."""

load("//loom/build_tools/bazel:defs.bzl", "loom_corpus_manifest")

CONTROL_CORPUS = loom_corpus_manifest(
    name = "control",
    package = "//loom/src/loom/test/corpus/control",
    srcs = [
        "branch/structured.loom",
        "loop/vector_recurrence.loom",
        "schedule/address_domains.loom",
        "schedule/ordered_read_ahead.loom",
        "schedule/unroll_scope.loom",
    ],
)
