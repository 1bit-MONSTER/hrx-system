#!/usr/bin/env python3
# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

from __future__ import annotations

import argparse
import json
import os
import shlex
import subprocess
import sys
import tempfile
from pathlib import Path


def parse_arguments(argv: list[str]) -> tuple[argparse.Namespace, list[str]]:
    if "--" not in argv:
        raise ValueError("missing '--' before compile arguments")
    delimiter = argv.index("--")
    parser = argparse.ArgumentParser()
    parser.add_argument("--clang-tidy", required=True, type=Path)
    parser.add_argument("--compiler", required=True, type=Path)
    parser.add_argument("--plugin", required=True, type=Path)
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--config-file", required=True, type=Path)
    parser.add_argument("--vfsoverlay", required=True, type=Path)
    parser.add_argument("--checks")
    parser.add_argument("--warnings-as-errors")
    parser.add_argument("--export-fixes", type=Path)
    parser.add_argument("--line-filter")
    parser.add_argument("--recursion-summary", type=Path)
    parser.add_argument("--suppress-recursion-diagnostics", action="store_true")
    parser.add_argument(
        "--allow-diagnostics",
        action="store_true",
        help="Return success when clang-tidy produced diagnostic fix artifacts.",
    )
    args = parser.parse_args(argv[:delimiter])
    return args, argv[delimiter + 1 :]


def write_empty_fixes(path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        "---\nMainSourceFile: ''\nDiagnostics: []\n...\n",
        encoding="utf-8",
    )


def main(argv: list[str]) -> int:
    try:
        args, compile_args = parse_arguments(argv)
    except ValueError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    command = [
        str(args.clang_tidy),
        f"--load={args.plugin}",
        f"--config-file={args.config_file}",
        f"--vfsoverlay={args.vfsoverlay}",
    ]
    if args.checks:
        command.append(f"--checks={args.checks}")
    command.append(str(args.source))
    if args.warnings_as_errors:
        command.append(f"--warnings-as-errors={args.warnings_as_errors}")
    if args.export_fixes:
        command.append(f"--export-fixes={args.export_fixes}")
    if args.line_filter:
        command.append(f"--line-filter={args.line_filter}")
    environment = os.environ.copy()
    if args.recursion_summary:
        environment["IREE_CLANG_TIDY_RECURSION_SUMMARY"] = str(args.recursion_summary)
    if args.suppress_recursion_diagnostics:
        environment["IREE_CLANG_TIDY_RECURSION_DIAGNOSTICS"] = "0"
    # Preserve the compiler's complete input spelling. FixedCompilationDatabase
    # strips positional inputs and appends an absolute path; clang-cl can parse
    # a POSIX path beginning with /w as a warning option instead of a source.
    # A command database retains the relative input from the native build action.
    # The actual compiler path also determines implicit SDK and standard-library
    # discovery, including headers installed beside a non-system toolchain.
    compiler_command = [str(args.compiler), *compile_args]
    with tempfile.TemporaryDirectory(prefix="iree-clang-tidy-") as directory:
        database = Path(directory) / "compile_commands.json"
        database.write_text(
            json.dumps(
                [
                    {
                        "directory": str(Path.cwd()),
                        "file": str(args.source),
                        "arguments": compiler_command,
                    }
                ]
            ),
            encoding="utf-8",
        )
        command.append(f"-p={directory}")
        completed = subprocess.run(
            command,
            env=environment,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
    output = completed.stdout
    args.output.write_text(output, encoding="utf-8")
    if (
        args.export_fixes
        and completed.returncode == 0
        and not args.export_fixes.exists()
    ):
        write_empty_fixes(args.export_fixes)
    if completed.returncode != 0:
        if args.allow_diagnostics and args.export_fixes and args.export_fixes.exists():
            return 0
        print("clang-tidy command failed:", file=sys.stderr)
        print(shlex.join(command), file=sys.stderr)
        print(
            "compiler command:",
            shlex.join(compiler_command),
            file=sys.stderr,
        )
        if output:
            print(output, file=sys.stderr, end="" if output.endswith("\n") else "\n")
        return completed.returncode
    if args.recursion_summary and not args.recursion_summary.is_file():
        print(
            "clang-tidy did not produce the requested recursion summary: "
            f"{args.recursion_summary}",
            file=sys.stderr,
        )
        return 1
    return completed.returncode


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
