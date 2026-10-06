# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Portable, linear-time batching of path arguments."""

from __future__ import annotations

import subprocess

# CreateProcess limits its command line to 32,767 UTF-16 code units including
# the terminator. Keep one portable bound below that ceiling so repository-wide
# file checks have the same batching behavior on every host.
MAX_PORTABLE_COMMAND_LINE_UTF16_UNITS = 30_000


def command_line_utf16_units(command: list[str]) -> int:
    rendered_command = subprocess.list2cmdline(command)
    return len(rendered_command.encode("utf-16-le")) // 2 + 1


def batch_path_commands(
    command_prefix: list[str],
    paths: list[str],
    *,
    max_command_line_utf16_units: int = MAX_PORTABLE_COMMAND_LINE_UTF16_UNITS,
) -> list[list[str]]:
    if not command_prefix:
        raise ValueError("command prefix must not be empty")
    if max_command_line_utf16_units <= 0:
        raise ValueError("command-line limit must be positive")
    prefix_units = command_line_utf16_units(command_prefix)
    if prefix_units > max_command_line_utf16_units:
        raise ValueError("command prefix exceeds the portable command-line limit")

    commands: list[list[str]] = []
    command = list(command_prefix)
    prefix_length = len(command_prefix)
    current_units = prefix_units
    for path in paths:
        # Windows quotes each argument independently. Its terminating unit
        # accounts for the separator added before this argument instead.
        added_units = command_line_utf16_units([path])
        if current_units + added_units <= max_command_line_utf16_units:
            command.append(path)
            current_units += added_units
            continue
        if len(command) > prefix_length:
            commands.append(command)
        command = [*command_prefix, path]
        current_units = prefix_units + added_units
        if current_units > max_command_line_utf16_units:
            raise ValueError(f"path exceeds the portable command-line limit: {path}")
    if len(command) > prefix_length:
        commands.append(command)
    return commands
