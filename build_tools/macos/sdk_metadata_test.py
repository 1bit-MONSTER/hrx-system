# Copyright 2026 The IREE Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

import tempfile
import unittest
from pathlib import Path

from build_tools.macos.sdk_metadata import system_library_aliases


class SystemLibraryAliasesTest(unittest.TestCase):
    def test_preserves_the_selected_alias_family(self):
        with tempfile.TemporaryDirectory() as temporary:
            sdk = Path(temporary)
            libraries = sdk / "usr/lib"
            libraries.mkdir(parents=True)
            (libraries / "libSystem.B.tbd").touch()
            (libraries / "libSystem.tbd").symlink_to("libSystem.B.tbd")
            aliases = ("c", "dl", "info", "m", "poll", "proc", "pthread")
            for name in aliases:
                (libraries / f"lib{name}.tbd").symlink_to("libSystem.tbd")
            (libraries / "libc++.1.tbd").touch()
            (libraries / "libc++.tbd").symlink_to("libc++.1.tbd")
            (libraries / "libz.1.tbd").touch()
            (libraries / "libz.tbd").symlink_to("libz.1.tbd")
            self.assertEqual(
                system_library_aliases(
                    sdk, {"usr/lib/libSystem.tbd", "usr/lib/libc++.1.tbd"}
                ),
                {f"usr/lib/lib{name}.tbd" for name in aliases}
                | {"usr/lib/libSystem.tbd", "usr/lib/libc++.tbd"},
            )

    def test_new_and_retargeted_aliases_follow_the_current_sdk(self):
        with tempfile.TemporaryDirectory() as temporary:
            sdk = Path(temporary)
            libraries = sdk / "usr/lib"
            libraries.mkdir(parents=True)
            (libraries / "libSystem.B.tbd").touch()
            (libraries / "libOther.tbd").touch()
            alias = libraries / "libm.tbd"
            alias.symlink_to("libOther.tbd")
            inputs = {"usr/lib/libSystem.B.tbd"}
            self.assertEqual(system_library_aliases(sdk, inputs), set())
            alias.unlink()
            alias.symlink_to("libSystem.B.tbd")
            self.assertEqual(system_library_aliases(sdk, inputs), {"usr/lib/libm.tbd"})
            (libraries / "libc.tbd").symlink_to("libm.tbd")
            self.assertEqual(
                system_library_aliases(sdk, inputs),
                {"usr/lib/libc.tbd", "usr/lib/libm.tbd"},
            )


if __name__ == "__main__":
    unittest.main()
