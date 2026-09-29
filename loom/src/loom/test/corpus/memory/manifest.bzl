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
        "address/address.loom",
        "atomic/operations.loom",
        "buffer/allocation_freshness.loom",
        "buffer/boundaries.loom",
        "buffer/buffer_access.loom",
        "buffer/buffers.loom",
        "buffer/masked_memory.loom",
        "storage/word.loom",
        "view/access.loom",
        "view/boundary_transport.loom",
        "view/nested_selection.loom",
        "view/offset_recurrence.loom",
        "view/rotation.loom",
        "view/selected_crops.loom",
        "view/selection.loom",
    ],
)
