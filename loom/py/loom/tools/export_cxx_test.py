# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Public command behavior for readable C++ export."""

from __future__ import annotations

import io
from pathlib import Path

from loom.format.cxx.test_support import SOURCE_MODULE
from loom.tools.export_cxx import run


def test_reads_text_from_stdin_and_writes_cxx_to_stdout() -> None:
    stdout = io.StringIO()
    stderr = io.StringIO()

    exit_code = run(
        "-",
        "-",
        stdin=io.BytesIO(SOURCE_MODULE.encode()),
        stdout=stdout,
        stderr=stderr,
    )

    assert exit_code == 0
    assert "void copy_one(" in stdout.getvalue()
    assert stderr.getvalue() == ""


def test_writes_an_output_file(tmp_path: Path) -> None:
    input_path = tmp_path / "input.loom"
    output_path = tmp_path / "output.cxx"
    input_path.write_text(SOURCE_MODULE)

    exit_code = run(
        str(input_path),
        str(output_path),
        stdin=io.BytesIO(),
        stdout=io.StringIO(),
        stderr=io.StringIO(),
    )

    assert exit_code == 0
    assert "void copy_one(" in output_path.read_text()


def test_reports_invalid_input_without_a_traceback() -> None:
    stderr = io.StringIO()

    exit_code = run(
        "-",
        "-",
        stdin=io.BytesIO(b"not loom"),
        stdout=io.StringIO(),
        stderr=stderr,
    )

    assert exit_code == 2
    assert stderr.getvalue().startswith("loom-export-cxx: error: ")
