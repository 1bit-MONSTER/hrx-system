# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Small deterministic pretty printer for generated Loom C++."""

from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True, slots=True)
class _Call:
    prefix: str
    arguments: tuple[str, ...]
    suffix: str


def _delimiter_pairs(text: str) -> dict[int, int]:
    pairs: dict[int, int] = {}
    stack: list[int] = []
    quote: str | None = None
    escaped = False
    for index, character in enumerate(text):
        if quote is not None:
            if escaped:
                escaped = False
            elif character == "\\":
                escaped = True
            elif character == quote:
                quote = None
            continue
        if character in {'"', "'"}:
            quote = character
        elif character == "(":
            stack.append(index)
        elif character == ")" and stack:
            pairs[stack.pop()] = index
    return pairs


def _split_arguments(text: str) -> tuple[str, ...]:
    if not text.strip():
        return ()
    parts: list[str] = []
    start = 0
    parentheses = 0
    braces = 0
    brackets = 0
    angles = 0
    quote: str | None = None
    escaped = False
    for index, character in enumerate(text):
        if quote is not None:
            if escaped:
                escaped = False
            elif character == "\\":
                escaped = True
            elif character == quote:
                quote = None
            continue
        if character in {'"', "'"}:
            quote = character
        elif character == "(":
            parentheses += 1
        elif character == ")":
            parentheses -= 1
        elif character == "{":
            braces += 1
        elif character == "}":
            braces -= 1
        elif character == "[":
            brackets += 1
        elif character == "]":
            brackets -= 1
        elif character == "<":
            angles += 1
        elif character == ">" and angles:
            angles -= 1
        elif (
            character == ","
            and parentheses == 0
            and braces == 0
            and brackets == 0
            and angles == 0
        ):
            parts.append(text[start:index].strip())
            start = index + 1
    parts.append(text[start:].strip())
    return tuple(parts)


def _outer_call(text: str) -> _Call | None:
    stripped = text.rstrip()
    terminal = len(stripped)
    while terminal and stripped[terminal - 1] in ";,":
        terminal -= 1
    if not terminal or stripped[terminal - 1] != ")":
        return None
    pairs = _delimiter_pairs(stripped)
    for opening, closing in pairs.items():
        if closing != terminal - 1:
            continue
        prefix = stripped[: opening + 1]
        callee = prefix[:-1].rstrip()
        if not callee or not (callee[-1].isalnum() or callee[-1] in "_>])"):
            return None
        if prefix.lstrip().startswith(("if (", "for (", "while (", "switch (")):
            return None
        return _Call(
            prefix=prefix,
            arguments=_split_arguments(stripped[opening + 1 : closing]),
            suffix=stripped[closing:],
        )
    return None


def _split_binary(text: str) -> tuple[str, str] | None:
    terminal = len(text)
    while terminal and text[terminal - 1] in ";,":
        terminal -= 1
    suffix = text[terminal:]
    expression = text[:terminal]
    depth = 0
    quote: str | None = None
    escaped = False
    candidates: list[int] = []
    for index, character in enumerate(expression):
        if quote is not None:
            if escaped:
                escaped = False
            elif character == "\\":
                escaped = True
            elif character == quote:
                quote = None
            continue
        if character in {'"', "'"}:
            quote = character
        elif character in "({[":
            depth += 1
        elif character in ")}]":
            depth -= 1
        elif depth == 0 and expression[index : index + 3] in {
            " + ",
            " - ",
            " * ",
            " / ",
            " | ",
            " & ",
        }:
            candidates.append(index)
    if not candidates:
        return None
    split = candidates[-1]
    return expression[: split + 2].rstrip(), expression[split + 3 :].lstrip() + suffix


class CxxWriter:
    """Emits stable C++ with bounded, structure-aware source lines."""

    def __init__(self, *, width: int = 100) -> None:
        self.lines: list[str] = []
        self.indent = 0
        self.width = width

    def _append(self, text: str, indent: int) -> None:
        self.lines.append("  " * indent + text if text else "")

    def _emit_attributes(self, text: str, indent: int) -> bool:
        if not text.startswith("[[") or not text.endswith("]]"):
            return False
        items = _split_arguments(text[2:-2])
        if len(items) < 2:
            return False
        self._append(f"[[{items[0]},", indent)
        for item in items[1:-1]:
            self._append(f"  {item},", indent)
        self._append(f"  {items[-1]}]]", indent)
        return True

    def _emit_initializer(self, text: str, indent: int) -> bool:
        opening = text.find("{")
        closing = text.rfind("}")
        if opening < 0 or closing < opening:
            return False
        items = _split_arguments(text[opening + 1 : closing])
        if len(items) < 4:
            return False
        self._append(text[: opening + 1], indent)
        current = ""
        for index, item in enumerate(items):
            fragment = item + ("," if index + 1 != len(items) else "")
            candidate = fragment if not current else f"{current} {fragment}"
            if current and 2 * (indent + 1) + len(candidate) > self.width:
                self._append(current, indent + 1)
                current = fragment
            else:
                current = candidate
        if current:
            self._append(current + text[closing:], indent + 1)
        return True

    def _emit(self, text: str, indent: int) -> None:
        if not text:
            self._append("", indent)
            return
        if 2 * indent + len(text) <= self.width:
            self._append(text, indent)
            return
        if self._emit_attributes(text, indent):
            return

        call = _outer_call(text)
        if call is not None and call.arguments:
            if " = " in call.prefix and (
                len(call.arguments) == 1 or 2 * indent + len(call.prefix) > self.width
            ):
                assignment, expression = text.split(" = ", 1)
                self._append(f"{assignment} =", indent)
                self._emit(expression, indent + 2)
                return
            self._append(call.prefix, indent)
            for argument in call.arguments[:-1]:
                self._emit(argument + ",", indent + 2)
            self._emit(call.arguments[-1], indent + 2)
            self.lines[-1] += call.suffix
            return

        if self._emit_initializer(text, indent):
            return
        if " = " in text:
            assignment, expression = text.split(" = ", 1)
            self._append(f"{assignment} =", indent)
            self._emit(expression, indent + 2)
            return
        binary = _split_binary(text)
        if binary is not None:
            self._append(binary[0], indent)
            self._emit(binary[1], indent)
            return
        self._append(text, indent)

    def line(self, text: str = "") -> None:
        lines = text.splitlines() or [""]
        for line in lines:
            self._emit(line, self.indent)

    def attributes(self, attributes: list[str]) -> None:
        self.line("[[" + ", ".join(attributes) + "]]")

    def template_call(
        self, callee: str, template_arguments: list[str], arguments: list[str]
    ) -> None:
        one_line = (
            f"{callee}<"
            + ", ".join(template_arguments)
            + ">("
            + ", ".join(arguments)
            + ");"
        )
        if 2 * self.indent + len(one_line) <= self.width:
            self.line(one_line)
            return
        self.line(f"{callee}<")
        self.indent += 2
        for index, argument in enumerate(template_arguments):
            if index + 1 == len(template_arguments):
                call_arguments = ", ".join(arguments)
                self.line(f"{argument}>({call_arguments});")
            else:
                self.line(f"{argument},")
        self.indent -= 2

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
