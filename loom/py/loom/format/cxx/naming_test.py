# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Tests for stable C++ identifier planning."""

import pytest

from loom.format.cxx.naming import clean_identifier


@pytest.mark.parametrize(
    ("source", "expected"),
    [
        ("value$12", "value"),
        ("value_loom_dollar_12", "value"),
        ("class", "class_value"),
        ("42", "v42"),
        ("left-right", "left_right"),
    ],
)
def test_clean_identifier(source: str, expected: str) -> None:
    assert clean_identifier(source, "fallback") == expected
