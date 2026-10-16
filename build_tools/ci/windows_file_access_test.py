# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

from __future__ import annotations

import datetime as dt
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from build_tools.ci import windows_diagnostics as diagnostics

# This dependency holds a real Win32 handle until the test explicitly releases
# it. Read/write sharing matches a readable file whose deletion is blocked.
FILE_HOLDER = r"""
import ctypes
import sys
from ctypes import wintypes

kernel = ctypes.WinDLL("kernel32", use_last_error=True)
kernel.CreateFileW.argtypes = [
    wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, wintypes.LPVOID,
    wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE,
]
kernel.CreateFileW.restype = wintypes.HANDLE
kernel.CloseHandle.argtypes = [wintypes.HANDLE]
kernel.CloseHandle.restype = wintypes.BOOL
handle = kernel.CreateFileW(sys.argv[1], 0x80000000, int(sys.argv[2]), None, 3, 0, None)
if handle == ctypes.c_void_p(-1).value:
    raise ctypes.WinError(ctypes.get_last_error())
try:
    print("ready", flush=True)
    sys.stdin.readline()
finally:
    if not kernel.CloseHandle(handle):
        raise ctypes.WinError(ctypes.get_last_error())
"""


@unittest.skipUnless(os.name == "nt", "requires native Windows file sharing")
class WindowsFileAccessTest(unittest.TestCase):
    def capture(self, root: Path, path: Path, label: str) -> tuple[dict, dict]:
        artifacts = root / label
        artifacts.mkdir()
        (artifacts / "output.log").write_text(
            f"ERROR: Couldn't delete action output directory: {path} (Permission denied)\n",
            encoding="utf-8",
        )
        diagnostics.capture_failure(
            artifacts, root, os.environ, None, started_at=dt.datetime.now(dt.UTC)
        )
        manifest = json.loads((artifacts / "manifest.json").read_text())
        self.assertNotIn("capture_error", manifest)
        tool = next(
            entry
            for entry in manifest["tools"]
            if "windows-file-access" in entry["output"]
        )
        text = (artifacts / tool["output"]).read_text(encoding="utf-8")
        self.assertEqual(tool.get("returncode"), 0, (tool, text))
        observations = [json.loads(line) for line in text.splitlines()]
        access = observations[0]["file_access"]
        self.assertEqual(access["path"], str(path))
        users = observations[1]["resource_users"] if len(observations) > 1 else {}
        return access, users

    def test_live_resource_owner_and_access_recover_after_release(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = root / "action stderr \N{GREEK SMALL LETTER LAMDA}"
            contents = b"compiler diagnostic\r\n"
            path.write_bytes(contents)
            for sharing, expected_read in ((3, 0), (0, 32)):
                with self.subTest(sharing=sharing):
                    child = subprocess.Popen(
                        [sys.executable, "-c", FILE_HOLDER, str(path), str(sharing)],
                        stdin=subprocess.PIPE,
                        stdout=subprocess.PIPE,
                        stderr=subprocess.PIPE,
                        text=True,
                    )
                    try:
                        self.assertEqual(child.stdout.readline(), "ready\n")
                        access, users = self.capture(root, path, f"held-{sharing}")
                        self.assertEqual(access["read"]["winerror"], expected_read)
                        self.assertEqual(access["delete"]["winerror"], 32)
                        for operation in (
                            "start_session",
                            "register_resources",
                            "get_list",
                            "end_session",
                        ):
                            self.assertEqual(users[operation]["winerror"], 0, users)
                        owner = next(
                            entry
                            for entry in users["processes"]
                            if entry["pid"] == child.pid
                        )
                        self.assertGreater(owner["start_time_100ns"], 0)
                        self.assertTrue(owner["application_name"])
                    finally:
                        _, errors = child.communicate("release\n")
                        self.assertEqual(child.returncode, 0, errors)
                    access, users = self.capture(root, path, f"released-{sharing}")
                    self.assertEqual(access["read"]["winerror"], 0)
                    self.assertEqual(access["delete"]["winerror"], 0)
                    self.assertEqual(users["get_list"]["winerror"], 0, users)
                    self.assertNotIn(
                        child.pid, [entry["pid"] for entry in users["processes"]]
                    )
                    self.assertEqual(path.read_bytes(), contents)

    def test_missing_path_retains_native_error(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = root / "absent"
            access, users = self.capture(root, path, "missing")
            self.assertEqual(access["read"]["winerror"], 2)
            self.assertEqual(access["delete"]["winerror"], 2)
            self.assertEqual(users, {})
            self.assertFalse(path.exists())


if __name__ == "__main__":
    unittest.main()
