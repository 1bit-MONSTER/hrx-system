# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Tests for portable path command-line batching."""

import subprocess
import unittest
from unittest import mock

from build_tools.devtools import command_line


class CommandLineTest(unittest.TestCase):
    def test_path_commands_are_batched_below_portable_command_line_limit(self):
        command_prefix = ["loom-format", "--check"]
        paths = ["loom/a.loom", "loom/b.loom", "loom/c.loom"]
        single_path_limit = command_line.command_line_utf16_units(
            [*command_prefix, paths[0]]
        )

        commands = command_line.batch_path_commands(
            command_prefix,
            paths,
            max_command_line_utf16_units=single_path_limit,
        )

        self.assertEqual(
            commands,
            [[*command_prefix, path] for path in paths],
        )
        for command in commands:
            self.assertLessEqual(
                command_line.command_line_utf16_units(command),
                single_path_limit,
            )

    def test_path_command_batching_rejects_one_oversized_path(self):
        command_prefix = ["loom-format", "--check"]
        prefix_limit = command_line.command_line_utf16_units(command_prefix)

        with self.assertRaisesRegex(
            ValueError, "path exceeds the portable command-line limit"
        ):
            command_line.batch_path_commands(
                command_prefix,
                ["loom/a.loom"],
                max_command_line_utf16_units=prefix_limit,
            )

    def test_path_batches_preserve_quoting_and_utf16_boundaries(self):
        command_prefix = ["C:/Program Files/loom-format", "--check"]
        paths = [
            "loom/with spaces.loom",
            'loom/with"quote.loom',
            "loom/back\\slash.loom",
            "loom/trailing space\\",
            "loom/non-bmp-\U0001f600.loom",
            "loom/tab\tname.loom",
            "",
        ]
        minimum_limit = max(
            command_line.command_line_utf16_units([*command_prefix, path])
            for path in paths
        )
        full_limit = command_line.command_line_utf16_units([*command_prefix, *paths])
        for limit in range(minimum_limit, full_limit + 1):
            with self.subTest(limit=limit):
                commands = command_line.batch_path_commands(
                    command_prefix,
                    paths,
                    max_command_line_utf16_units=limit,
                )
                self.assertEqual(
                    [path for command in commands for path in command[2:]], paths
                )
                for index, command in enumerate(commands):
                    self.assertEqual(command[:2], command_prefix)
                    self.assertLessEqual(
                        command_line.command_line_utf16_units(command), limit
                    )
                    if index + 1 < len(commands):
                        self.assertGreater(
                            command_line.command_line_utf16_units(
                                [*command, commands[index + 1][2]]
                            ),
                            limit,
                        )

    def test_path_batching_serializes_each_argument_once(self):
        command_prefix = ["loom-format", "--check"]
        paths = [f"loom/path_{index}.loom" for index in range(2000)]
        with mock.patch.object(
            command_line.subprocess,
            "list2cmdline",
            wraps=subprocess.list2cmdline,
        ) as render:
            commands = command_line.batch_path_commands(command_prefix, paths)

        self.assertEqual([path for command in commands for path in command[2:]], paths)
        self.assertEqual(
            sum(len(call.args[0]) for call in render.call_args_list),
            len(command_prefix) + len(paths),
        )

    def test_path_batching_validates_prefix_even_without_paths(self):
        for prefix, limit, error in (
            ([], 100, "command prefix must not be empty"),
            (["tool"], 0, "command-line limit must be positive"),
            (["tool"], 4, "command prefix exceeds the portable command-line limit"),
        ):
            with self.subTest(prefix=prefix, limit=limit):
                with self.assertRaisesRegex(ValueError, error):
                    command_line.batch_path_commands(
                        prefix, [], max_command_line_utf16_units=limit
                    )
        self.assertEqual(command_line.batch_path_commands(["tool"], []), [])


if __name__ == "__main__":
    unittest.main()
