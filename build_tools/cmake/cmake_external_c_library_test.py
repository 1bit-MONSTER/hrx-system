# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Checks compiler policy and source freshness across a nested C build."""

import shutil
import tempfile
import unittest
from pathlib import Path

from build_tools.cmake.test_environment import (
    CONFIGURATION,
    REPO_ROOT,
    build_project,
    configure_project,
    run_command,
)


class CMakeExternalCLibraryTest(unittest.TestCase):
    def test_toolchain_lists_flags_and_source_revisions(self):
        with tempfile.TemporaryDirectory(prefix="cmake external library ") as temporary:
            root = Path(temporary)
            source = root / "source with spaces"
            build = root / "build with spaces"
            shutil.copytree(
                REPO_ROOT / "build_tools/cmake/testdata/external_c_library", source
            )
            configure_project(source, build)
            program = (build / f"program-{CONFIGURATION}.txt").read_text().strip()
            build_project(build, "probe")
            self.assertEqual(run_command(program), "42\n")
            library = source / "upstream" / "value.c"
            library.write_text(library.read_text().replace("+ 1;", "+ 2;"))
            build_project(build, "probe")
            self.assertEqual(run_command(program), "43\n")


if __name__ == "__main__":
    unittest.main()
