# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Mutates source-returned image plans through admission and span validation."""

import struct
import subprocess
import sys
import tempfile
from pathlib import Path

if __name__ == "__main__":
    executable = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="media-plan-fuzz-") as temporary:
        corpus = Path(temporary)
        (corpus / "empty").write_bytes(bytes(3))
        # Two ordered markers with distinct opaque metadata and logical spans.
        records = struct.pack("<12I", 0, 1, 2, 3, 48, 2, 1, 5, 1, 4, 50, 1)
        spans = struct.pack("<6I", 0, 1, 3, 1, 4, 4)
        (corpus / "pair").write_bytes(bytes([2, 10, 0]) + records + b"abc" + spans)
        (corpus / "suffix").write_bytes(bytes([2, 10, 13]) + records + b"abc" + spans)
        subprocess.run(
            [str(executable), "-runs=4096", "-max_len=16384", str(corpus)],
            cwd=corpus,
            check=True,
        )
