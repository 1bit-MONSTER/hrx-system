# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Exercises documentation rules with normal Semgrep path filtering."""

from __future__ import annotations

import json
import subprocess
import tempfile
import unittest
from pathlib import Path


class AmdDocumentationRulesTest(unittest.TestCase):
    def test_documentation_dependencies_and_scope(self):
        hardware_vocabulary = "amd-reference.no-library-vocabulary"
        hardware_link = "amd-reference.no-local-implementation-links"
        library_cts = "libamdf.docs.no-cts-dependencies"
        fixtures = {
            "docs/reference/amd/gpu/packets.md": [
                ("The packet selects objects and affects products.", None),
                ("libamdf selects this packet.", hardware_vocabulary),
                ("`AMDF_GPU_QUEUE_FLAG_NONE` is public.", hardware_vocabulary),
                ("`amdf_gpu_queue_create` is public.", hardware_vocabulary),
                ("A CTS observation is not a native contract.", hardware_vocabulary),
                ("An experimental cts result is not a contract.", hardware_vocabulary),
                ("[Queue](https://example.org/driver/src/queue.c)", None),
                ("[ABI]: https://example.org/native/include/hsa.h", None),
                ("[Queue](../../../../runtime/src/queue.c)", hardware_link),
                ("[ABI]: <../../../../include/amdf/gpu.h>", hardware_link),
                ("[Storage](../src/storage.c#ownership)", hardware_link),
                ("[Notes](../../../../.notes/session.md)", hardware_link),
                ("[Source](file:///tmp/src/native.c)", hardware_link),
                ("[Notes]: <file:///tmp/.notes/evidence.md>", hardware_link),
                ("[Header](/include/native.h)", hardware_link),
                ("[Source]: /src/native.c", hardware_link),
                ("[Cache](cache.md#src-fields)", None),
                ("[Local section](#include-layout)", None),
                ("[External source](https://example.org/src/source.c)", None),
            ],
            "docs/reference/amd/xdna/README.md": [
                ("LIBAMDF support is not a hardware contract.", hardware_vocabulary),
            ],
            "libamdf/docs/memory.md": [
                ("libamdf exposes `AMDF_MEMORY_FLAG_NONE`.", None),
                ("[API](../include/amdf/memory.h)", None),
                ("This selects objects with no side effects.", None),
                ("CTS qualifies the API.", library_cts),
                ("[Cases](../cts/README.md)", library_cts),
                ("[Experimental cases](../experimental/cts/README.md)", library_cts),
                ("[Cases]: ../cts/experimental/README.md", library_cts),
            ],
            "libamdf/docs/experimental/example.rst": [
                ("Experimental CTS results.", library_cts),
            ],
            "libamdf/README.md": [
                ("libamdf exposes `amdf_query_api`.", None),
                ("The CTS checks this contract.", library_cts),
            ],
            "libamdf/cts/README.md": [("libamdf AMDF_API CTS", None)],
            "libamdf/experimental/cts/README.md": [("libamdf AMDF_API CTS", None)],
            "libamdf/README.extra.md": [("CTS", None)],
            "docs/reference/amd-other/README.md": [("libamdf AMDF_API CTS", None)],
            "docs/reference/other/README.md": [("libamdf AMDF_API CTS", None)],
        }
        expected = set()
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            for path, lines in fixtures.items():
                target = root / path
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text("\n".join(text for text, _ in lines) + "\n")
                expected.update(
                    (path, number, rule)
                    for number, (_, rule) in enumerate(lines, 1)
                    if rule is not None
                )
            result = subprocess.run(
                [
                    "semgrep",
                    "scan",
                    "--metrics=off",
                    "--disable-version-check",
                    "--strict",
                    "--json",
                    "--jobs",
                    "1",
                    "--no-rewrite-rule-ids",
                    "--config",
                    str(Path(__file__).with_name("amd-docs.yml").resolve()),
                    "--",
                    *fixtures,
                ],
                cwd=root,
                capture_output=True,
                text=True,
                check=False,
            )
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        report = json.loads(result.stdout)
        self.assertEqual(report["errors"], [])
        actual = {
            (finding["path"], finding["start"]["line"], finding["check_id"])
            for finding in report["results"]
        }
        self.assertEqual(actual, expected)


if __name__ == "__main__":
    unittest.main()
