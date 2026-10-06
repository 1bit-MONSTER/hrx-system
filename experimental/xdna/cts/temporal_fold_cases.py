# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Independent ordered F32 and byte oracles for wide native temporal folds."""

import struct
import sys
from pathlib import Path


def round_f32(value):
    return struct.unpack("<f", struct.pack("<f", value))[0]


def contributions(fold, element, first_copy):
    category = element % 8
    if category == 0:
        return (-0.0, -0.0, -0.0) if first_copy else (0.0, 0.0, 0.0)
    if category == 1:
        return (float(1 << 24), 1.0, -float(1 << 24))
    if category == 2:
        return (float(1 << 24), -float(1 << 24), 1.0)
    if category == 3:
        return (-float(1 << 24), -1.0, float(1 << 24))
    if category == 4:
        return (float(fold * 1024 + element), 0.25, -0.125)
    if category == 5:
        return (0.5 * (element + 1), -float(fold + 1), 0.0625)
    if category == 6:
        return (2.0**-125, 2.0**-125, 2.0**-125)
    return (-0.0, 0.0, -0.0)


def main():
    case = sys.argv[1]
    width, input_width, record_count = {
        "first-copy": (1024, 1024, 1),
        "wide": (1024, 1024, 3),
        "tail": (80, 80, 3),
        "large-first-copy": (4096, 1024, 1),
        "large": (4096, 1024, 3),
        "mixed": (144, 144, 3),
    }[case]
    directory = Path(sys.argv[2])
    directory.mkdir(parents=True, exist_ok=True)
    guard = bytes([0xA5]) * 64
    inputs = bytearray(guard)
    expected = bytearray(guard)
    scalar_expected = bytearray(guard)
    for fold in range(8):
        values = [
            contributions(fold, element, record_count == 1)
            for element in range(input_width)
        ]
        for record in range(record_count):
            header = bytearray(64)
            struct.pack_into("<II", header, 0, (fold + record) & 1, width * 4)
            inputs.extend(header)
            inputs.extend(
                struct.pack(f"<{input_width}f", *(value[record] for value in values))
            )
        for element in range(width):
            # Each expanded 4 KiB block has a different lane permutation, so
            # misaddressed private spans cannot pass by repeating one block.
            value = values[(element % input_width) ^ (element // input_width)]
            # The first contribution is copied, not added to positive zero.
            accumulated = round_f32(value[0])
            for contribution in value[1:record_count]:
                accumulated = round_f32(accumulated + contribution)
            expected.extend(struct.pack("<f", accumulated))
        scalar = round_f32(values[4][0])
        for contribution in values[4][1:record_count]:
            scalar = round_f32(scalar + contribution)
        scalar_expected.extend(struct.pack("<f", scalar))
    inputs.extend(guard)
    expected.extend(guard)
    (directory / "input.bin").write_bytes(inputs)
    (directory / "output.bin").write_bytes(bytes([0xA5]) * len(expected))
    (directory / "expected.bin").write_bytes(expected)
    if case == "mixed":
        scalar_expected.extend(guard)
        (directory / "scalar-output.bin").write_bytes(
            bytes([0xA5]) * len(scalar_expected)
        )
        (directory / "scalar-expected.bin").write_bytes(scalar_expected)


if __name__ == "__main__":
    main()
