# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Tests for deterministic C++ layout."""

from loom.format.cxx.printer import CxxWriter


def test_wraps_generated_structure_without_parsing_cxx() -> None:
    writer = CxxWriter(width=50)
    writer.attributes(
        [
            "loom::kernel",
            "loom::workgroup_count(4, 1, 1)",
            "loom::workgroup_size(64, 1, 1)",
        ]
    )
    writer.line("auto result = namespace::function(first_argument, second_argument);")
    writer.line(
        "unsigned long long offset = (first_index * 100ull) + (second_index * 100ull);"
    )
    writer.template_call(
        "loom::kernel::barrier",
        ["loom::memory_space::workgroup", "loom::atomic::scope::workgroup"],
        [],
    )

    assert (
        "\n".join(writer.lines)
        == """\
[[loom::kernel,
  loom::workgroup_count(4, 1, 1),
  loom::workgroup_size(64, 1, 1)]]
auto result = namespace::function(
    first_argument,
    second_argument);
unsigned long long offset =
    (first_index * 100ull) +
    (second_index * 100ull);
loom::kernel::barrier<
    loom::memory_space::workgroup,
    loom::atomic::scope::workgroup>();"""
    )


def test_keeps_adjacent_control_flow_clauses_together() -> None:
    writer = CxxWriter()
    writer.open("if (condition)")
    writer.line("result = first;")
    writer.continue_block("else")
    writer.line("result = second;")
    writer.close()

    assert (
        "\n".join(writer.lines)
        == """\
if (condition) {
  result = first;
} else {
  result = second;
}"""
    )
