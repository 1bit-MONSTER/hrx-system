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
        "bitfield.loom",
        "boolean_logic.loom",
        "constants.loom",
        "conversion.loom",
        "conversion_paths.loom",
        "division.loom",
        "exponential.loom",
        "extrema.loom",
        "float_comparison.loom",
        "float_selectors.loom",
        "floating.loom",
        "integer.loom",
        "integer_boundaries.loom",
        "integer_comparison.loom",
        "logarithm.loom",
        "narrow.loom",
        "narrow_arithmetic.loom",
        "roots.loom",
        "rounding.loom",
        "selection.loom",
        "turns.loom",
        "vector_fields.loom",
        "vector_floating.loom",
        "vector_indexing.loom",
        "vector_reductions.loom",
        "vector_selection.loom",
    ],
)
