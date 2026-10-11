#!/usr/bin/env bash

set -euo pipefail

if [[ "$#" -ne 3 ]]; then
  printf "expected source map, --probe, and fixture path; got %d args\n" \
    "$#" >&2
  exit 1
fi

if [[ "$1" != --source-prefix-map=* ]]; then
  printf "expected source prefix map, got %s\n" "$1" >&2
  exit 1
fi

if [[ "$2" != "--probe" ]]; then
  printf "expected forwarded --probe flag, got %s\n" "$2" >&2
  exit 1
fi

fixture="$3"
source_root="${1#--source-prefix-map=}"
source_root="${source_root%=}"
source_root="${source_root%/}"
expected_suffix="/loom/build_tools/bazel/test/roundtrip.loom-test"
if [[ "${fixture}" != "${source_root}${expected_suffix}" ]]; then
  printf "fixture %s does not match source root %s\n" \
    "${fixture}" "${source_root}" >&2
  exit 1
fi

if [[ ! -f "${fixture}" ]]; then
  printf "fixture path does not exist: %s\n" "${fixture}" >&2
  exit 1
fi
