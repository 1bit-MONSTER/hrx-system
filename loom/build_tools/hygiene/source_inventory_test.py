# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Real filesystem tests for the declared source view."""

import tempfile
import unittest
from pathlib import Path

from source_inventory import inventory


class SourceInventoryTest(unittest.TestCase):
    def test_membership_does_not_depend_on_contents_or_build_registration(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            sources = root / "loom/nested package"
            sources.mkdir(parents=True)
            source = sources / "unregistered.loom"
            source.write_text("first", encoding="utf-8")
            first = inventory(root)
            self.assertEqual(
                first["sources"], ["loom/nested package/unregistered.loom"]
            )
            self.assertEqual(
                first["directories"],
                {
                    "loom": {"nested package": True},
                    "loom/nested package": {"unregistered.loom": False},
                },
            )
            source.write_text("different contents", encoding="utf-8")
            self.assertEqual(inventory(root), first)
            consumer = sources / "consumer.loom-test"
            consumer.touch()
            self.assertEqual(
                inventory(root)["sources"],
                [
                    "loom/nested package/consumer.loom-test",
                    "loom/nested package/unregistered.loom",
                ],
            )
            consumer.unlink()
            self.assertEqual(inventory(root), first)
            source.rename(sources / "renamed.loom")
            self.assertEqual(
                inventory(root)["sources"], ["loom/nested package/renamed.loom"]
            )

    def test_hidden_scratch_and_other_source_types_are_not_inputs(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            scratch = root / "loom/.notes"
            scratch.mkdir(parents=True)
            (scratch / "broken.loom").touch()
            (root / "loom/.hidden.loom").touch()
            (root / "loom/source.c").touch()
            self.assertEqual(inventory(root)["sources"], [])
            self.assertEqual(
                inventory(root)["directories"], {"loom": {"source.c": False}}
            )

    def test_missing_source_root_fails(self):
        with tempfile.TemporaryDirectory() as temporary:
            with self.assertRaises(FileNotFoundError):
                inventory(Path(temporary))

    def test_symlink_directory_cannot_hide_coverage_or_create_a_cycle(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "loom").mkdir()
            link = root / "loom/cycle"
            link.symlink_to(root / "loom", target_is_directory=True)
            with self.assertRaisesRegex(ValueError, "directory must not be a symlink"):
                inventory(root)

    def test_missing_source_symlink_fails(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "loom").mkdir()
            (root / "loom/missing.loom").symlink_to(root / "absent")
            with self.assertRaisesRegex(ValueError, "source must be a regular file"):
                inventory(root)


if __name__ == "__main__":
    unittest.main()
