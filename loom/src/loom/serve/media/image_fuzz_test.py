# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Runs a bounded codec facade mutation pass from the checked-in seed corpus."""

import subprocess
import sys
import tempfile
from pathlib import Path

if __name__ == "__main__":
    executable = Path(sys.argv[1]).absolute()
    seeds = Path(sys.argv[2]).absolute().parent
    with tempfile.TemporaryDirectory(prefix="image-fuzz-") as corpus:
        subprocess.run(
            [str(executable), "-runs=4096", "-max_len=65536", corpus, str(seeds)],
            cwd=corpus,
            check=True,
        )
