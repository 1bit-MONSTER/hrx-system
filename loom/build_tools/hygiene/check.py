# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Runs a read-only source check against a declared source manifest."""

from __future__ import annotations

import argparse
import json
import subprocess
from pathlib import Path

from build_tools.devtools.command_line import batch_path_commands


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tool", type=Path, required=True)
    parser.add_argument("--sources", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    arguments = parser.parse_args()
    tool = arguments.tool.absolute()
    source_manifest = arguments.sources.absolute()
    output = arguments.output.absolute()
    paths = json.loads(source_manifest.read_text(encoding="utf-8"))
    prefix = [str(tool), "--check-templates", "--template-root=."]
    # Keep the portable command bound while passing every source as its own
    # argument. Native flag files cannot carry positional input paths.
    failed = False
    for command in batch_path_commands(prefix, paths):
        failed = (
            subprocess.run(command, cwd=source_manifest.parent).returncode != 0
            or failed
        )
    if failed:
        return 1
    output.write_text("PASS\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
