# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Input-format parity for readable C++ rendering."""

from loom.format.bytecode.writer import write_module
from loom.format.cxx.registry import load_module, source_ops
from loom.format.cxx.renderer import render_module
from loom.format.cxx.test_support import SOURCE_MODULE


def test_text_and_bytecode_render_identically() -> None:
    text_module = load_module(SOURCE_MODULE.encode(), filename="copy_one.loom")
    bytecode = write_module(text_module, op_decls=source_ops())
    bytecode_module = load_module(bytecode, filename="copy_one.loombc")

    assert render_module(bytecode_module) == render_module(text_module)
