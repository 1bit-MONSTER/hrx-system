# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Independent byte oracle for selected and loop-carried C++ pointers."""

import struct
import sys
from pathlib import Path


def main():
    directory = Path(sys.argv[1])
    directory.mkdir(parents=True, exist_ok=True)
    inputs = []
    expected = []
    for record, count in enumerate((0, 1, 2, 3, 4, 5, 6, 7, 8, 2)):
        words = [
            ((record + 1) * 0x10203041 + word * 0x110001) & 0xFFFFFFFF
            for word in range(256)
        ]
        words[:2] = [count, record]
        inputs.extend(words)
        result = [0x6BAD0000 + word for word in range(256)]
        for word in range(16):
            choose_input = (record + word) % 2 != 0
            original = words[16 + word] if choose_input else result[64 + word]
            updated = original ^ 0x7F00FF80
            result[128 + word] = updated
            result[160 + word] = updated if choose_input else words[16 + word]
            result[192 + word] = result[64 + word] if choose_input else updated
            if not choose_input:
                result[64 + word] = updated
        first_block = words[32:48]
        second_block = result[96:112]
        result[224:240] = first_block if count % 2 == 0 else second_block
        result[240:256] = second_block if count % 2 == 0 else first_block
        expected.extend(result)
    guard = bytes([0xA5]) * 64
    (directory / "input.bin").write_bytes(struct.pack(f"<{len(inputs)}I", *inputs))
    (directory / "output.bin").write_bytes(bytes([0xA5]) * (len(expected) * 4) + guard)
    (directory / "expected.bin").write_bytes(
        struct.pack(f"<{len(expected)}I", *expected) + guard
    )


if __name__ == "__main__":
    main()
