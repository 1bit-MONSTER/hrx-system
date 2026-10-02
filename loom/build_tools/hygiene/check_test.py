# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Runs the action adapter against the production native template checker."""

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from python.runfiles import runfiles


class CheckTest(unittest.TestCase):
    def test_native_diagnostics_and_success_stamp(self):
        resolver = runfiles.Create()
        runner = resolver.Rlocation(sys.argv[1])
        checker = resolver.Rlocation(sys.argv[2])
        with tempfile.TemporaryDirectory(prefix="loom hygiene ") as temporary:
            root = Path(temporary)
            source_root = root / "loom/source"
            source_root.mkdir(parents=True)
            template = source_root / "template.loom-test"
            consumer = source_root / "consumer.loom-test"
            contents = "func.def @example() {\n  func.return\n}\n"
            template.write_text(contents, encoding="utf-8")
            consumer.write_text(
                "// TEMPLATE: loom/source/template.loom-test\n\n" + contents,
                encoding="utf-8",
            )
            manifest = root / "sources.json"
            manifest.write_text(
                json.dumps(
                    [str(path.relative_to(root)) for path in (consumer, template)]
                ),
                encoding="utf-8",
            )
            output = root / "passed"
            command = [
                runner,
                "--tool",
                checker,
                "--sources",
                str(manifest),
                "--output",
                str(output),
            ]
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(output.read_text(encoding="utf-8"), "PASS\n")
            output.unlink()

            # The consumer is unchanged: its source dependency changed.
            template.write_text(
                contents.replace(
                    "  func.return", "  // changed template\n  func.return"
                ),
                encoding="utf-8",
            )
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("is stale relative to", result.stdout + result.stderr)
            self.assertIn("consumer.loom-test", result.stdout + result.stderr)
            self.assertFalse(output.exists())

            template.write_text(contents, encoding="utf-8")
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            output.unlink()
            template.unlink()
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("NOT_FOUND", result.stdout + result.stderr)
            self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
