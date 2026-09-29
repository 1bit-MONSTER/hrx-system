# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Authored source inventory for memory semantic conformance."""

load("//loom/build_tools/bazel:defs.bzl", "loom_corpus_manifest")

MEMORY_CORPUS = loom_corpus_manifest(
    name = "memory",
    package = "//loom/src/loom/test/corpus/memory",
    srcs = [
        "address.loom",
        "allocation_freshness.loom",
        "atomics.loom",
        "buffer_access.loom",
        "buffers.loom",
        "masked_memory.loom",
        "memory_boundaries.loom",
        "nested_view_selection.loom",
        "view_access.loom",
        "view_boundary_transport.loom",
        "view_offset_recurrence.loom",
        "view_rotation.loom",
        "view_selected_crops.loom",
        "view_selection.loom",
        "word.loom",
    ],
)
