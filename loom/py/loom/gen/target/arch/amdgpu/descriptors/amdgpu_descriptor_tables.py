# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Generator: AMDGPU ISA XML -> global descriptor table family."""

from __future__ import annotations

import argparse
import sys
from collections.abc import Sequence
from pathlib import Path


def _ensure_runtime_py_on_path() -> None:
    runtime_py = Path(__file__).resolve().parents[6]
    runtime_py_string = str(runtime_py)
    if runtime_py_string not in sys.path:
        sys.path.insert(0, runtime_py_string)


_ensure_runtime_py_on_path()

from loom.gen.support.files import write_text_file  # noqa: E402
from loom.gen.support.generated_file import line_comment_header  # noqa: E402
from loom.gen.target.arch.amdgpu.descriptors.amdgpu_lower_capabilities import (  # noqa: E402
    generate_lower_capability_header,
)
from loom.gen.target.arch.amdgpu.descriptors.amdgpu_planning_table_inputs import (  # noqa: E402
    AmdgpuPlanningTableInputs,
    load_amdgpu_planning_table_inputs,
)
from loom.gen.target.arch.amdgpu.descriptors.amdgpu_vopd_component_tables import (  # noqa: E402
    amdgpu_vopd_instruction_names_by_isa_key,
    generate_vopd_component_table_outputs,
)
from loom.gen.target.arch.amdgpu.descriptors.amdgpu_wait_packet_tables import (  # noqa: E402
    generate_wait_packet_table_outputs,
)
from loom.gen.target.arch.amdgpu.refs.amdgpu_target_refs import (  # noqa: E402
    generate_target_ref_outputs,
    select_target_ref_descriptor_set_infos,
)
from loom.target.arch.amdgpu.descriptors.matrix import MATRIX_OPERAND_PLACEMENT_QUALIFICATIONS  # noqa: E402
from loom.target.arch.amdgpu.target_info import (  # noqa: E402
    amdgpu_descriptor_set_ordinal,
    amdgpu_target_descriptor_set_key,
    sorted_processor_infos,
    sorted_target_infos,
)
from loom.target.low_descriptors import OperandRole  # noqa: E402


def _instruction_placement_bindings(inputs: AmdgpuPlanningTableInputs) -> str:
    """Joins qualifications to the actual generated descriptor/processor rows."""
    lookups: dict[tuple[str, str, int], list[int]] = {}
    bindings: list[tuple[int, int, tuple[str, str, int]]] = []
    ranges: list[tuple[int, int]] = []
    qualifications = set(MATRIX_OPERAND_PLACEMENT_QUALIFICATIONS)
    processors = sorted_processor_infos()
    unknown = {row[0] for row in qualifications} - {info.processor for info in processors}
    if unknown:
        raise ValueError(f"unknown placement qualification processors: {sorted(unknown)}")
    for processor in processors:
        first_binding = len(bindings)
        descriptor_set_keys = sorted({amdgpu_target_descriptor_set_key(target, processor) for target in sorted_target_infos() if target.processor == processor.processor})
        for name, subgroup_size, descriptor_key in sorted(qualifications):
            if name != processor.processor:
                continue
            for key in descriptor_set_keys:
                descriptor_set = inputs.descriptor_sets_by_key[key]
                ordinal = next((i for i, descriptor in enumerate(descriptor_set.descriptors) if descriptor.key == descriptor_key), None)
                if ordinal is None:
                    raise ValueError(f"placement qualification '{descriptor_key}' is absent from '{key}'")
                descriptor = descriptor_set.descriptors[ordinal]
                operands = tuple(operand for operand in descriptor.operands if operand.role is OperandRole.OPERAND)
                if tuple(operand.field_name for operand in operands) != ("lhs", "rhs", "acc") or any(
                    operand.unit_count < minimum or not any(alt.reg_class is not None for alt in operand.reg_alts) for operand, minimum in zip(operands, (1, 1, 2), strict=True)
                ):
                    raise ValueError(f"placement qualification '{descriptor_key}' requires register lhs/rhs/acc coordinates and two accumulator units")
                # Descriptor vocabulary may be shared without sharing measured
                # processor/subgroup qualifications.
                lookup_key = (key, name, subgroup_size)
                lookup = lookups.setdefault(lookup_key, [0] * len(descriptor_set.descriptors))
                lookup[ordinal] = 1
                binding = (amdgpu_descriptor_set_ordinal(key), subgroup_size, lookup_key)
                if binding not in bindings[first_binding:]:
                    bindings.append(binding)
        ranges.append((first_binding, len(bindings) - first_binding))
    lines = [*line_comment_header("//", generator="loom.gen.target.arch.amdgpu.descriptors.amdgpu_descriptor_tables"), ""]
    lookup_names = {key: f"kInstructionPreferenceIndices{i}" for i, key in enumerate(lookups)}
    for key, lookup in lookups.items():
        lines.append(f"static const uint16_t {lookup_names[key]}[{len(lookup)}] = {{")
        lines.extend(f"    [{i}] = {value}," for i, value in enumerate(lookup) if value)
        lines.append("};")
    lines.append("static const loom_amdgpu_placement_binding_t kInstructionPreferenceBindings[] = {")
    lines.extend(f"    {{{ordinal}, {subgroup}, {lookup_names[key]}}}," for ordinal, subgroup, key in bindings)
    lines.append("};")
    lines.append(f"static const loom_amdgpu_placement_binding_range_t kInstructionPreferenceRanges[{len(processors)}] = {{")
    lines.extend(f"    [{i}] = {{{first}, {count}}}," for i, (first, count) in enumerate(ranges) if count)
    lines.append("};")
    return "\n".join(lines) + "\n"


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=("Generate global AMDGPU descriptor-derived tables from one ISA corpus."))
    parser.add_argument(
        "--isa-xml",
        action="append",
        default=[],
        help="ISA XML fact source as <key>:<path>.",
    )
    parser.add_argument(
        "--descriptor-set",
        action="append",
        default=[],
        help="Descriptor-set key to include in target-reference tables.",
    )
    parser.add_argument("--target-ref-header", type=Path, required=True)
    parser.add_argument("--target-ref-source", type=Path, required=True)
    parser.add_argument(
        "--target-ref-public-header",
        default="loom/target/arch/amdgpu/refs/target_refs.h",
    )
    parser.add_argument("--wait-packet-source", type=Path, required=True)
    parser.add_argument("--vopd-source", type=Path, required=True)
    parser.add_argument("--placement-bindings", type=Path, required=True)
    parser.add_argument("--lower-capabilities-header", type=Path, required=True)
    args = parser.parse_args(argv)

    inputs = load_amdgpu_planning_table_inputs(
        args.isa_xml,
        amdgpu_vopd_instruction_names_by_isa_key(),
    )
    selected_descriptor_set_infos = select_target_ref_descriptor_set_infos(args.descriptor_set)
    selected_descriptor_sets = tuple(inputs.descriptor_sets_by_key[info.key] for info in selected_descriptor_set_infos)
    generate_target_ref_outputs(
        public_header=args.target_ref_public_header,
        descriptor_set_infos=selected_descriptor_set_infos,
        descriptor_sets_by_key=inputs.descriptor_sets_by_key,
        header_path=args.target_ref_header,
        source_path=args.target_ref_source,
    )
    generate_lower_capability_header(selected_descriptor_sets, args.lower_capabilities_header)
    generate_wait_packet_table_outputs(
        inputs,
        source_path=args.wait_packet_source,
    )
    generate_vopd_component_table_outputs(
        inputs,
        source_path=args.vopd_source,
    )
    write_text_file(args.placement_bindings, _instruction_placement_bindings(inputs))
    return 0


if __name__ == "__main__":
    sys.exit(main())
