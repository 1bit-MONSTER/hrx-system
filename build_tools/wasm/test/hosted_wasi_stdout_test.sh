#!/usr/bin/env bash
# Copyright 2026 The IREE Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

set -euo pipefail

# A merged pipe matches Bazel test capture, including Node's warning stream.
if output=$("$1" --large-output 2>&1); then
  payload="${output}"
  # Remove short warning lines without a quadratic longest-prefix glob.
  while [[ "${payload}" == *$'\n'* ]]; do
    payload="${payload#*$'\n'}"
  done
else
  printf 'hosted WASI process failed; captured %s bytes\n' "${#output}" >&2
  printf '%s\n' "${output:0:512}" >&2
  exit 1
fi

if [[ ${#payload} -ne 1048576 ]]; then
  echo "expected one MiB of output, got ${#payload} bytes" >&2
  exit 1
fi
# Doubling constructs the expected bytes without a large glob search.
expected=x
for ((doubling = 0; doubling < 20; ++doubling)); do
  expected="${expected}${expected}"
done
if [[ "${payload}" != "${expected}" ]]; then
  echo "hosted WASI output contains unexpected bytes" >&2
  exit 1
fi
