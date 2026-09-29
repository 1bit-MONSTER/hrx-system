# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Tests for Loom's dependency-free pytest-style runner."""

import unittest

import pytest

from loom.tools import pytest_style_runner


def test_include_exhaustive_flag_is_explicit() -> None:
    options = pytest_style_runner._parse_runner_args(
        ("--include-exhaustive", "example.tests")
    )
    assert options.module_names == ("example.tests",)
    assert options.include_exhaustive

    default_options = pytest_style_runner._parse_runner_args(("example.tests",))
    assert not default_options.include_exhaustive


def test_exhaustive_marker_skips_default_execution(
    capsys: pytest.CaptureFixture[str],
) -> None:
    calls: list[str] = []

    @pytest.mark.exhaustive
    def sample() -> None:
        calls.append("ran")

    test_case = next(
        pytest_style_runner._expand_test_callable(__name__, "test_sample", sample)
    )
    with pytest.raises(unittest.SkipTest, match="--include-exhaustive"):
        pytest_style_runner._run_test_case(test_case, include_exhaustive=False)
    assert not calls

    pytest_style_runner._run_test_case(test_case, include_exhaustive=True)
    assert calls == ["ran"]
    assert capsys.readouterr().out.count("RUN ") == 2
