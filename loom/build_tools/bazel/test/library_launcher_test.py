# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Exercises argument handling through generated launchers and real tools."""

import argparse
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


class LibraryLauncherTest(unittest.TestCase):
    paths: dict[str, Path]

    def run_launcher(self, name, *arguments):
        return subprocess.run(
            [str(self.paths[name]), *arguments],
            capture_output=True,
            text=True,
            check=False,
        )

    def read_reports(self, completed):
        self.assertEqual(completed.returncode, 0, completed.stderr)
        reports = []
        remaining = completed.stdout.strip()
        decoder = json.JSONDecoder()
        while remaining:
            report, length = decoder.raw_decode(remaining)
            reports.append(report)
            remaining = remaining[length:].lstrip()
        self.assertTrue(reports, completed.stdout)
        return reports

    def test_runtime_selection_overrides_declared_case(self):
        for name in ("correctness", "combined"):
            with self.subTest(launcher=name):
                reports = self.read_reports(
                    self.run_launcher(
                        name,
                        "--case=@benchmark_case",
                        "--case=@scalar_case",
                        "--sample=0",
                    )
                )
                self.assertEqual(reports[0]["case_count"], 1)
                self.assertEqual(
                    [sample["case"] for sample in reports[0]["samples"]],
                    ["scalar_case"],
                )
                if name == "combined":
                    self.assertEqual(
                        {row["case"] for row in reports[1:] if "case" in row},
                        {"scalar_case"},
                    )
                    self.assertEqual(reports[-1]["row"], "summary")
                else:
                    self.assertEqual(len(reports), 1)

    def test_correctness_owned_options_do_not_reach_benchmark(self):
        completed = self.run_launcher(
            "combined",
            "--case=@scalar_case",
            "--allow-failure=@scalar_case=EXPECT/003",
            f"--library={self.paths['library']}",
        )
        reports = self.read_reports(completed)
        self.assertEqual(reports[-1]["row"], "summary")

    def test_correctness_failure_prevents_benchmark(self):
        for argument in (
            "--case=@missing_case",
            "--xfail=@benchmark_case=EXPECT/003",
            "",
        ):
            with self.subTest(argument=argument):
                completed = self.run_launcher("combined", argument)
                self.assertNotEqual(completed.returncode, 0)
                self.assertNotIn('"row":', completed.stdout)

    def test_shared_flagfile_reaches_both_runners(self):
        with tempfile.TemporaryDirectory() as directory:
            flagfile = Path(directory) / "selection.flags"
            flagfile.write_text("--case=@scalar_case\n")
            reports = self.read_reports(
                self.run_launcher("combined", f"--flagfile={flagfile}")
            )
            self.assertEqual(reports[0]["samples"][0]["case"], "scalar_case")
            self.assertEqual(
                {row["case"] for row in reports[1:] if "case" in row},
                {"scalar_case"},
            )

    def test_default_smoke_still_runs(self):
        reports = self.read_reports(self.run_launcher("combined"))
        self.assertEqual(reports[0]["samples"][0]["case"], "benchmark_case")
        self.assertEqual(reports[-1]["row"], "summary")
        self.assertEqual(
            reports[-1]["summary"]["planned"]["selected_benchmark_count"], 1
        )

    def test_quoted_argument_boundaries(self):
        with tempfile.TemporaryDirectory(prefix="loom output 'quoted' ") as directory:
            reports = self.read_reports(
                self.run_launcher("combined", f"--file-output-root={directory}")
            )
            self.assertEqual(reports[-1]["row"], "summary")

    def test_unknown_arguments_fail_in_every_launcher(self):
        for name in ("combined", "correctness", "format", "lint", "plan", "artifacts"):
            with self.subTest(launcher=name):
                completed = self.run_launcher(name, "--unknown-launcher-option")
                self.assertNotEqual(completed.returncode, 0)
                self.assertTrue(completed.stderr)

    def test_single_tool_launchers_forward_help(self):
        for name in ("format", "lint", "plan"):
            with self.subTest(launcher=name):
                completed = self.run_launcher(name, "--help")
                self.assertEqual(completed.returncode, 0, completed.stderr)
                self.assertIn("usage", (completed.stdout + completed.stderr).lower())

    def test_artifact_check_without_arguments(self):
        completed = self.run_launcher("artifacts")
        self.assertEqual(completed.returncode, 0, completed.stderr)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    for name in (
        "combined",
        "correctness",
        "format",
        "lint",
        "plan",
        "artifacts",
        "library",
    ):
        parser.add_argument(f"--{name}", type=Path, required=True)
    paths, arguments = parser.parse_known_args()
    LibraryLauncherTest.paths = {
        name: path.absolute() for name, path in vars(paths).items()
    }
    unittest.main(argv=[sys.argv[0], *arguments])
