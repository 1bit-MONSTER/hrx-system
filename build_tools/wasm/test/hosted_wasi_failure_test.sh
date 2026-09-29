#!/usr/bin/env bash
# Copyright 2026 The IREE Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

set -euo pipefail

set +e
"$1" --fail
status=$?
set -e

if [[ "${status}" -ne 17 ]]; then
  echo "expected hosted WASI exit status 17, got ${status}" >&2
  exit 1
fi
