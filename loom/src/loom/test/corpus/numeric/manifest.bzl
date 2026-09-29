# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Authored source inventory for numeric semantic conformance."""

load("//loom/build_tools/bazel:defs.bzl", "loom_corpus_manifest")

NUMERIC_CORPUS = loom_corpus_manifest(
    name = "numeric",
    package = "//loom/src/loom/test/corpus/numeric",
    srcs = [
        "extrema.loom",
    ],
)
