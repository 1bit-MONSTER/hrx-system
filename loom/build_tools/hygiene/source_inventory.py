# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Enumerates source membership without reading source contents."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path


def inventory(repository_root: Path) -> dict:
    directories = {}
    sources = []
    pending = [repository_root / "loom"]
    while pending:
        directory = pending.pop()
        entries = {}
        with os.scandir(directory) as iterator:
            for entry in iterator:
                if entry.name.startswith(".") or entry.name == "__pycache__":
                    continue
                is_directory = entry.is_dir()
                entries[entry.name] = is_directory
                path = Path(entry.path)
                relative_path = path.relative_to(repository_root).as_posix()
                if is_directory:
                    if entry.is_symlink():
                        raise ValueError(
                            f"source directory must not be a symlink: {relative_path}"
                        )
                    pending.append(path)
                elif path.suffix in (".loom", ".loom-test"):
                    if not entry.is_file():
                        raise ValueError(
                            f"source must be a regular file: {relative_path}"
                        )
                    sources.append(relative_path)
        directories[directory.relative_to(repository_root).as_posix()] = entries
    return {"directories": directories, "sources": sorted(sources)}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("repository_root", type=Path)
    arguments = parser.parse_args()
    print(json.dumps(inventory(arguments.repository_root), sort_keys=True))


if __name__ == "__main__":
    main()
