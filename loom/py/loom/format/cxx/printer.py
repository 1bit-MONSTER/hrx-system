# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Small deterministic writer for generated Loom C++."""

from __future__ import annotations


class CxxWriter:
    """Emits stable C++ without imposing an arbitrary source width."""

    def __init__(self) -> None:
        self.lines: list[str] = []
        self.indent = 0

    def line(self, text: str = "") -> None:
        lines = text.splitlines() or [""]
        for line in lines:
            self.lines.append("  " * self.indent + line if line else "")

    def attributes(self, attributes: list[str]) -> None:
        self.line("[[" + ", ".join(attributes) + "]]")

    def template_call(
        self, callee: str, template_arguments: list[str], arguments: list[str]
    ) -> None:
        self.line(f"{callee}<{', '.join(template_arguments)}>({', '.join(arguments)});")

    def open(self, text: str) -> None:
        self.line(text + " {")
        self.indent += 1

    def close(self, suffix: str = "") -> None:
        self.indent -= 1
        self.line("}" + suffix)

    def continue_block(self, text: str) -> None:
        """Closes one block and opens an adjacent clause at the same depth."""
        self.indent -= 1
        self.line(f"}} {text} {{")
        self.indent += 1
