# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Independent boundary oracle for native immutable gather widths."""

import struct
import sys
from pathlib import Path

TABLE = tuple(float(value) for value in range(1, 33))
I32_INDEX_PATTERNS = (
    tuple(range(16)),
    tuple(reversed(range(16))),
    (0, 7, 8, 15) * 4,
    (7, 8, 6, 9, 15, 0, 14, 1, 11, 4, 12, 3, 5, 10, 2, 13),
)
BF16_INDEX_PATTERNS = (
    tuple(range(32)),
    tuple(reversed(range(32))),
    (0, 15, 16, 31) * 8,
    tuple((lane * 13 + 7) % 32 for lane in range(32)),
)
F64_INDEX_PATTERNS = (
    tuple(range(8)),
    tuple(reversed(range(8))),
    (0, 3, 4, 7) * 2,
    (7, 0, 5, 2, 6, 1, 4, 3),
)
RHS_VALUES = (1.0, -0.5, -1.5, 0.25, 0.5, -0.25, 2.0, -1.0)


def bf16_bits(value):
    return struct.unpack("<I", struct.pack("<f", value))[0] >> 16


def main():
    directory = Path(sys.argv[1])
    directory.mkdir(parents=True, exist_ok=True)
    inputs = bytearray()
    expected = bytearray()
    table_bf16_bits = tuple(bf16_bits(value) for value in TABLE)
    table_bytes = struct.pack("<32H", *table_bf16_bits)
    for record, indices in enumerate(I32_INDEX_PATTERNS):
        # High bits establish that the runtime mask, rather than the fixture,
        # supplies the proven table bound consumed by native gather selection.
        raw_indices = [
            index | (((record + lane + 1) & 0xFFFF) << 16)
            for lane, index in enumerate(indices)
        ]
        bf16_indices = BF16_INDEX_PATTERNS[record]
        f64_indices = F64_INDEX_PATTERNS[record]
        rhs = [RHS_VALUES[(record + lane) % len(RHS_VALUES)] for lane in range(32)]
        initial = [record * 8.0 - 6.0 + lane * 0.25 for lane in range(16)]
        inputs += struct.pack("<16I", *raw_indices)
        inputs += bytes(index | 0xA0 for index in bf16_indices)
        inputs += struct.pack(
            "<8I",
            *(
                index | (((record + lane + 0x51) & 0xFFFF) << 16)
                for lane, index in enumerate(f64_indices)
            ),
        )
        inputs += struct.pack("<32H", *(bf16_bits(value) for value in rhs))
        inputs += struct.pack("<16f", *initial)

        result = []
        for lane, index in enumerate(indices):
            left = TABLE[index * 2]
            right = TABLE[index * 2 + 1]
            result.append(
                initial[lane] + left * rhs[lane * 2] + right * rhs[lane * 2 + 1]
            )
        expected += struct.pack("<16f", *result)
        expected += struct.pack(
            "<32H", *(table_bf16_bits[index] for index in bf16_indices)
        )
        for index in f64_indices:
            expected += table_bytes[index * 8 : index * 8 + 8]

    guard = bytes([0xA5]) * 64
    (directory / "input.bin").write_bytes(inputs + guard)
    (directory / "output.bin").write_bytes(bytes([0xCD]) * len(expected) + guard)
    (directory / "expected.bin").write_bytes(expected + guard)


if __name__ == "__main__":
    main()
