# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Runs a read-only source check against a declared source manifest."""

from __future__ import annotations

import argparse
import json
import re
import subprocess
from pathlib import Path

from build_tools.devtools.command_line import batch_path_commands

_SCENARIO_CORPUS_PREFIXES = tuple(
    f"loom/src/loom/test/corpus/{family}/"
    for family in ("control", "function", "kernel", "memory", "numeric")
)
_CHECK_RECORD_PATTERN = re.compile(r"^\s*check\.(case|scenario)(?:\s|$)")


def check_module_headers(paths: list[str], root: Path) -> bool:
    """Rejects source-style license headers on authored Loom modules."""
    ok = True
    for path in paths:
        if Path(path).suffix != ".loom":
            continue
        source = root / path
        if not source.is_file():
            continue
        with source.open(encoding="utf-8") as source_file:
            for line_number, line in enumerate(source_file, start=1):
                stripped_line = line.strip()
                if not stripped_line:
                    continue
                if not stripped_line.startswith("//"):
                    break
                if stripped_line.startswith(
                    ("// Copyright ", "// SPDX-License-Identifier:")
                ):
                    print(
                        f"{path}:{line_number}: .loom modules must not carry "
                        "copyright or license headers"
                    )
                    ok = False
                    break
    return ok


def check_corpus_scenarios(paths: list[str], root: Path) -> bool:
    """Requires every manifest-owned corpus module to use scenarios."""
    ok = True
    for path in paths:
        if not path.startswith(_SCENARIO_CORPUS_PREFIXES) or not path.endswith(".loom"):
            continue
        source = root / path
        if not source.is_file():
            continue
        has_scenario = False
        for line_number, line in enumerate(
            source.read_text(encoding="utf-8").splitlines(), start=1
        ):
            match = _CHECK_RECORD_PATTERN.match(line)
            if not match:
                continue
            if match.group(1) == "scenario":
                has_scenario = True
            else:
                print(
                    f"{path}:{line_number}: manifest-owned corpus modules must "
                    "use check.scenario instead of check.case"
                )
                ok = False
        if not has_scenario:
            print(
                f"{path}:1: manifest-owned corpus modules must define at least "
                "one check.scenario"
            )
            ok = False
    return ok


def check_template_ownership(paths: list[str], root: Path) -> bool:
    """Rejects template containers that do not actually share source."""
    consumers: dict[str, list[str]] = {}
    for path in paths:
        source = root / path
        if not source.is_file():
            continue
        for line in source.read_text(encoding="utf-8").splitlines():
            if line.startswith("// TEMPLATE: "):
                template = line.removeprefix("// TEMPLATE: ").strip()
                consumers.setdefault(template, []).append(path)
                break

    ok = True
    for template, template_consumers in sorted(consumers.items()):
        template_path = root / template
        if (
            len(template_consumers) != 1
            or template_path.suffix != ".loom-test"
            or not template_path.is_file()
        ):
            continue
        print(
            f"{template_consumers[0]}:1: TEMPLATE source {template} has only "
            "one consumer; keep the source in its owning fixture"
        )
        ok = False
    return ok


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tool", type=Path, required=True)
    parser.add_argument("--sources", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument(
        "--check",
        choices=("templates", "format", "authoring", "repository"),
        required=True,
    )
    arguments = parser.parse_args()
    tool = arguments.tool.absolute()
    source_manifest = arguments.sources.absolute()
    output = arguments.output.absolute()
    if arguments.check == "repository":
        commands = [[str(tool), "--manifest", str(source_manifest)]]
    else:
        paths = json.loads(source_manifest.read_text(encoding="utf-8"))
        if arguments.check == "templates" and not check_template_ownership(
            paths, source_manifest.parent
        ):
            return 1
        if arguments.check == "authoring":
            valid_headers = check_module_headers(paths, source_manifest.parent)
            valid_scenarios = check_corpus_scenarios(paths, source_manifest.parent)
            if not valid_headers or not valid_scenarios:
                return 1
        flags = {
            "templates": ["--check-templates", "--template-root=."],
            "format": ["--check"],
            "authoring": [],
        }[arguments.check]
        commands = batch_path_commands([str(tool), *flags], paths)
    # Keep the portable command bound while passing every source as its own
    # argument. Native flag files cannot carry positional input paths.
    failed = False
    for command in commands:
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
