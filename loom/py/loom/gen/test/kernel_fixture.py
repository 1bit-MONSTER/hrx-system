# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""NPY-backed kernel checks with exact expected values and output guards."""

import os
import struct
from pathlib import Path

ELEMENTS = {"f16": ("e", "<f2", 2), "f32": ("f", "<f4", 4), "f64": ("d", "<f8", 8), "i8": ("b", "|i1", 1), "i16": ("h", "<i2", 2), "i32": ("i", "<i4", 4), "i64": ("q", "<i8", 8)}


def signed_bits(value, width):
    """Represent an integer bit pattern in signed literal/NumPy storage."""
    return (value + (1 << (width - 1))) % (1 << width) - (1 << (width - 1))


def rounded(value, element):
    code, _, _ = ELEMENTS[element]
    return struct.unpack("<" + code, struct.pack("<" + code, value))[0]


def write_npy(path, values, element):
    code, description, _ = ELEMENTS[element]
    header = repr({"descr": description, "fortran_order": False, "shape": (len(values),)})
    header += " " * ((64 - (10 + len(header) + 1) % 64) % 64) + "\n"
    path.write_bytes(b"\x93NUMPY\x01\x00" + struct.pack("<H", len(header)) + header.encode("ascii") + struct.pack(f"<{len(values)}{code}", *values))


class Arrays:
    """Owns fixture output files and their source-relative read paths."""

    def __init__(self, directory, source_directory):
        self.directory = directory
        self.source_directory = source_directory
        directory.mkdir(parents=True, exist_ok=True)

    def write(self, filename, values, element):
        path = self.directory / filename
        write_npy(path, values, element)
        return Path(os.path.relpath(path, self.source_directory)).as_posix()


class Case:
    def __init__(self, arrays, name, element, count):
        self.arrays = arrays
        self.name = name
        self.element = element
        self.count = count
        self.guard = "-123" if element.startswith("i") else "-123.0"
        self.lines = [f"check.case public @{name} {{"]
        self.lines.append(f"  %storage = check.generate.fill value({self.guard}) : tensor<{count + 32}x{element}>")
        _, _, width = ELEMENTS[element]
        self.lines.append(f"  %output = check.tensor.view %storage offset({16 * width}) : tensor<{count + 32}x{element}> -> tensor<{count}x{element}>")

    def array(self, name, values, element=None):
        element = element or self.element
        filename = f"{self.name}_{name}.npy"
        filename = self.arrays.write(filename, values, element)
        self.lines.append(f'  %{name} = check.file.read.npy path("{filename}") : tensor<{len(values)}x{element}>')

    def scalar(self, name, value, element):
        self.lines.append(f"  %{name} = check.literal value({value}) : {element}")

    def launch(self, kernel, arguments, types):
        self.lines.append(f"  kernel.launch @{kernel}({arguments}) : ({types})")

    def finish(self, expected, tolerance=None):
        self.array("expected", expected)
        if tolerance is None:
            self.lines.append(f"  check.expect.bitwise actual(%output) expected(%expected) : tensor<{self.count}x{self.element}>")
        else:
            self.lines.append(f"  check.expect.close actual(%output) expected(%expected) atol({tolerance}) rtol({tolerance}) nan(same) : tensor<{self.count}x{self.element}>")
        _, _, width = ELEMENTS[self.element]
        for name, offset in [("prefix", 0), ("suffix", (self.count + 16) * width)]:
            self.lines.append(f"  %{name} = check.tensor.view %storage offset({offset}) : tensor<{self.count + 32}x{self.element}> -> tensor<16x{self.element}>")
        self.lines.append(f"  %guard = check.generate.fill value({self.guard}) : tensor<16x{self.element}>")
        for name in ["prefix", "suffix"]:
            self.lines.append(f"  check.expect.bitwise actual(%{name}) expected(%guard) : tensor<16x{self.element}>")
        self.lines.extend(["  check.return", "}", ""])
        return "\n".join(self.lines)
