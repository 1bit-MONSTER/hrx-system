# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Python rules with Loom package requirements."""

load(
    "//build_tools/bazel:python.bzl",
    "iree_py_binary",
    "iree_py_library",
    "iree_py_test",
)
load(
    "//loom/requirements:package_policy.bzl",
    "apply_loom_target_policy",
    "apply_loom_test_policy",
)

def loom_py_library(**kwargs):
    """Defines a Python library with Loom package build requirements.

    Args:
      **kwargs: Attributes forwarded to iree_py_library.
    """
    iree_py_library(**apply_loom_target_policy(kwargs))

def loom_py_binary(**kwargs):
    """Defines a Python binary with Loom package build requirements.

    Args:
      **kwargs: Attributes forwarded to iree_py_binary.
    """
    iree_py_binary(**apply_loom_target_policy(kwargs))

def loom_py_test(**kwargs):
    """Defines a Python test with Loom package build and run requirements.

    Args:
      **kwargs: Attributes forwarded to iree_py_test.
    """
    iree_py_test(**apply_loom_test_policy(kwargs))
