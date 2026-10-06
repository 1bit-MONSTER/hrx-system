# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Independent word oracle for shared streams with skewed native consumers."""

import struct
import sys
from pathlib import Path

WORD_MASK = (1 << 32) - 1
RECORD_COUNT = 33
RECORD_WORDS = 16


def packet(tag, stream, record):
    words = [
        (
            tag
            ^ ((stream + 1) * 0x85EBCA6B)
            ^ ((record + 1) * 0xC2B2AE35)
            ^ ((word + 1) * 0x9E3779B9)
        )
        & WORD_MASK
        for word in range(RECORD_WORDS)
    ]
    # The low bits vary work independently across streams and record positions.
    # Every record has sixteen distinct consumer counts, and the slowest
    # consumer changes as records advance.
    if tag == 0xA0000000:
        selector = ((stream + 3 * record) & 7) | (((record + stream) & 3) << 4)
    else:
        selector = ((stream ^ (record & 1)) << 3) | ((record * 5) & 7)
    words[0] = (words[0] & ~63) | selector
    return words


def expected_record(left, right):
    iterations = 64 * (1 + ((left[0] ^ right[0]) & 63))
    state = left[1] ^ right[1]
    for _ in range(iterations):
        # Masking after each left shift models a 32-bit word. Python's
        # nonnegative state makes the right shift explicitly logical.
        state ^= (state << 13) & WORD_MASK
        state ^= state >> 17
        state ^= (state << 5) & WORD_MASK
    return [((lhs + rhs) & WORD_MASK) ^ state for lhs, rhs in zip(left, right)]


def encode(records):
    return b"".join(struct.pack("<16I", *record) for record in records)


def main():
    directory = Path(sys.argv[1])
    directory.mkdir(parents=True, exist_ok=True)
    left = [
        [packet(0xA0000000, stream, record) for record in range(RECORD_COUNT)]
        for stream in range(8)
    ]
    right = [
        [packet(0xB0000000, stream, record) for record in range(RECORD_COUNT)]
        for stream in range(2)
    ]
    guard = bytes([0xA5]) * 64
    (directory / "a.bin").write_bytes(
        guard + encode(record for stream in left for record in stream) + guard
    )
    (directory / "b.bin").write_bytes(
        guard + encode(record for stream in right for record in stream) + guard
    )
    expected = encode(
        expected_record(left[column][record], right[row][record])
        for column in range(8)
        for row in range(2)
        for record in range(RECORD_COUNT)
    )
    (directory / "expected.bin").write_bytes(guard + expected + guard)
    (directory / "output.bin").write_bytes(bytes([0xA5]) * (len(expected) + 128))


if __name__ == "__main__":
    main()
