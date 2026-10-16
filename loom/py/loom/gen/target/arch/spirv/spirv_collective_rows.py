# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Typed instruction rows for the shared SPIR-V collective arithmetic."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path


def _ensure_runtime_py_on_path() -> None:
    runtime_py = str(Path(__file__).resolve().parents[5])
    if runtime_py not in sys.path:
        sys.path.insert(0, runtime_py)


_ensure_runtime_py_on_path()

from loom.gen.support.c import CIdentifierCase, c_identifier  # noqa: E402
from loom.gen.support.files import write_text_file  # noqa: E402
from loom.gen.support.generated_file import line_comment_header  # noqa: E402
from loom.gen.target.arch.spirv.spirv_workgroup_rows import generate_workgroup_tables  # noqa: E402
from loom.target.arch.spirv.descriptors import SPIRV_LOGICAL_CORE_DESCRIPTOR_SET  # noqa: E402
from loom.target.arch.spirv.subgroup import SPIRV_COLLECTIVE_EXTREMA_TYPES  # noqa: E402
from loom.target.low_descriptors import descriptor_set_relative_name  # noqa: E402


def generate_tables() -> str:
    descriptors = {row.key: row for row in SPIRV_LOGICAL_CORE_DESCRIPTOR_SET.descriptors}

    def reference(key: str) -> str:
        if key not in descriptors:
            raise ValueError(f"collective instruction has no descriptor: {key}")
        name = descriptor_set_relative_name(SPIRV_LOGICAL_CORE_DESCRIPTOR_SET, key)
        suffix = c_identifier(name, case=CIdentifierCase.UPPER)
        return f"{SPIRV_LOGICAL_CORE_DESCRIPTOR_SET.c_enum_prefix}_DESCRIPTOR_REF_{suffix}"

    lines = [
        *line_comment_header("//", generator="loom.gen.target.arch.spirv.spirv_collective_rows"),
        "",
        "static const loom_spirv_extrema_row_t kExtremaRows[][4] = {",
    ]
    for width in (16, 32, 64):
        lines.append("  {")
        for row in SPIRV_COLLECTIVE_EXTREMA_TYPES:
            if row.bit_count != width:
                continue
            lines.extend(("    {", f"      .bit_count = {width},", f"      .lane_count = {row.lane_count},"))
            for field, key in row.descriptors.items():
                lines.append(f"      .{field} = {reference(key)},")
            lines.append("      .subgroup = {")
            for mode in ("reduce", "clustered_reduce", "inclusive_scan", "exclusive_scan"):
                pair = ", ".join(reference(row.subgroup_key(mode, minimum=minimum)) for minimum in (True, False))
                lines.append(f"        {{{pair}}},")
            lines.extend(("      },", "    },"))
        lines.append("  },")
    lines.extend(("};", ""))
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser()
    outputs = parser.add_mutually_exclusive_group(required=True)
    outputs.add_argument("--tables", type=Path)
    outputs.add_argument("--workgroup-tables", type=Path)
    args = parser.parse_args()
    if args.tables:
        write_text_file(args.tables, generate_tables())
    else:
        write_text_file(args.workgroup_tables, generate_workgroup_tables())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
