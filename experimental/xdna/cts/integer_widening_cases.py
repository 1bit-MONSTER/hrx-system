# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Independent integer and quantization oracle across accumulator widening."""

import math
import struct
import sys
from pathlib import Path

INTEGER_VALUES = (
    0,
    1,
    0x7FFFFFFF,
    0x80000000,
    0xFFFFFFFF,
    0x12345678,
    0x87654321,
    0x40000000,
    0xC0000000,
)
QUANTIZE_VALUES = (
    -math.inf,
    -1.0,
    -0.5,
    0.5,
    1.0,
    5.0,
    6.0,
    math.nan,
    math.inf,
)
THRESHOLDS = (-1.0, 0.5, 5.0)


def main():
    directory = Path(sys.argv[1])
    directory.mkdir(parents=True, exist_ok=True)
    inputs = bytearray()
    expected = bytearray()
    for record in range(16):
        integers = [
            INTEGER_VALUES[(record + lane) % len(INTEGER_VALUES)] for lane in range(17)
        ]
        values = [
            QUANTIZE_VALUES[(record + lane) % len(QUANTIZE_VALUES)] for lane in range(9)
        ]

        source = bytearray([0xA5] * 256)
        source[0:68] = struct.pack("<17I", *integers)
        source[96:132] = struct.pack("<9f", *values)
        source[192:204] = struct.pack("<3f", *THRESHOLDS)
        inputs += source

        unsigned = integers
        signed = [
            value if value < (1 << 31) else value + 0xFFFFFFFF00000000
            for value in integers
        ]
        quantized = [
            0
            if math.isnan(value)
            else sum(threshold < value for threshold in THRESHOLDS)
            for value in values
        ]
        result = bytearray([0xA5] * 576)
        result[0:136] = struct.pack("<17Q", *unsigned)
        result[192:328] = struct.pack("<17Q", *signed)
        result[384:456] = struct.pack("<9Q", *quantized)
        expected += result

    # Binding tails expose DMA writes beyond the declared pipeline views.
    guard = bytes([0xA5]) * 64
    (directory / "input.bin").write_bytes(inputs + guard)
    (directory / "output.bin").write_bytes(bytes([0xA5]) * len(expected) + guard)
    (directory / "expected.bin").write_bytes(expected + guard)


if __name__ == "__main__":
    main()
