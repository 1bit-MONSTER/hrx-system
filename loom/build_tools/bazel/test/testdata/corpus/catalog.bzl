# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Shared catalog fixture for Loom corpus rule analysis tests."""

load("//loom/build_tools/bazel:defs.bzl", "loom_corpus_catalog")
load("//loom/build_tools/bazel/test/testdata/corpus/source:manifest.bzl", "SAMPLE_CORPUS")

TEST_CORPUS = loom_corpus_catalog([
    SAMPLE_CORPUS,
])
