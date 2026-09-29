# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Target-neutral source catalog for Loom semantic conformance."""

load("//loom/build_tools/bazel:defs.bzl", "loom_corpus_catalog")
load("//loom/src/loom/test/corpus/numeric:manifest.bzl", "NUMERIC_CORPUS")

LOOM_CORPUS = loom_corpus_catalog([
    NUMERIC_CORPUS,
])
