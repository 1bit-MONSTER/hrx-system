# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Independent raw-bit oracles for native BF16 matrix transposition."""

import struct
import sys
from pathlib import Path

SPECIAL_BITS = (
    0x0000,
    0x8000,
    0x0001,
    0x8001,
    0x007F,
    0x807F,
    0x0080,
    0x8080,
    0x3F80,
    0xBF80,
    0x3FC0,
    0xBFC0,
    0x7F7F,
    0xFF7F,
    0x7F80,
    0xFF80,
    0x7F81,
    0xFF81,
    0x7FC1,
    0xFFC1,
    0x3E80,
    0xBE80,
    0x4000,
    0xC000,
    0x1234,
    0x5678,
    0x9ABC,
    0xDEF0,
    0x5555,
    0xAAAA,
    0x2468,
    0x1357,
)


def transpose(words):
    result = [0] * 64
    for row in range(8):
        for column in range(8):
            result[8 * column + row] = words[8 * row + column]
    return result


def make_words(changed):
    # The odd multiplier permutes all 65,536 bit patterns for the changed case.
    exhaustive = [
        (position * 4051 + 0x9E37) & 0xFFFF if changed else position
        for position in range(65536)
    ]
    assert len(set(exhaustive)) == 65536
    special = tuple(reversed(SPECIAL_BITS)) if changed else SPECIAL_BITS
    words = list(exhaustive)
    for rotation in range(len(special)):
        words.extend(special[(lane + rotation) % len(special)] for lane in range(64))
    assert len(words) == 1056 * 64
    return words


def main():
    directory = Path(sys.argv[1])
    directory.mkdir(parents=True, exist_ok=True)
    head_guard = bytes([0xD3]) * 64
    tail_guard = bytes([0xA5]) * 64
    for name, changed in (("original", False), ("changed", True)):
        words = make_words(changed)
        expected = []
        for start in range(0, len(words), 64):
            packet = words[start : start + 64]
            result = transpose(packet)
            # Check the scalar coordinate oracle independently of its loops.
            assert result == [packet[(lane % 8) * 8 + lane // 8] for lane in range(64)]
            assert transpose(result) == packet
            expected.extend(result)
        source = struct.pack(f"<{len(words)}H", *words)
        result = struct.pack(f"<{len(expected)}H", *expected)
        (directory / f"{name}-input.bin").write_bytes(head_guard + source + tail_guard)
        (directory / f"{name}-expected.bin").write_bytes(
            head_guard + result + tail_guard
        )
    (directory / "output.bin").write_bytes(
        head_guard + bytes([0x5A]) * (1056 * 128) + tail_guard
    )


if __name__ == "__main__":
    main()
