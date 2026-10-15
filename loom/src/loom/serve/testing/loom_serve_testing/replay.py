# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Replay a frozen multi-model JSONL workload against a running HTTP server."""

import argparse
import json
import sys
from pathlib import Path

from loom_serve_testing import workload


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", required=True, type=workload.origin)
    parser.add_argument("--workload", required=True, type=Path)
    parser.add_argument("--concurrency", required=True, type=int)
    arguments = parser.parse_args(argv)
    if arguments.concurrency < 1:
        parser.error("--concurrency must be positive")
    try:
        with arguments.workload.open() as source:
            requests = workload.parse(source)
    except (OSError, ValueError) as error:
        parser.error(str(error))

    def emit(record):
        print(json.dumps(record, allow_nan=False), flush=True)

    emit(
        {
            "event": "workload",
            "format": 1,
            "requests": len(requests),
            "concurrency": arguments.concurrency,
            "sha256": workload.digest(
                [
                    [
                        request.identity,
                        request.path,
                        json.loads(request.body),
                        dict(request.headers),
                        request.predecessor,
                        request.delay_ns,
                    ]
                    for request in requests
                ]
            ),
        }
    )
    summary = workload.replay(arguments.server, requests, arguments.concurrency, emit)
    emit(summary)
    return int(any(group["failed"] or group["blocked"] for group in summary["classes"]))


if __name__ == "__main__":
    sys.exit(main())
