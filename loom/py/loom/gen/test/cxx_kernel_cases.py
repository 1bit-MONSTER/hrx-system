# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""C++ kernel inputs with independent numerical references."""

import argparse
import math
import struct
from pathlib import Path

from loom.gen.test.kernel_fixture import Arrays, Case, signed_bits


def pack_q4k(scale, minimum, group_scales, group_minimums, codes):
    """Pack logical Q4_K fields into its 144-byte storage layout."""
    scale0 = bytes((group_scales[group] & 15) | ((group_scales[group] >> 4) << 4) | ((group_scales[group + 4] >> 4) << 6) for group in range(4))
    scale1 = bytes((group_minimums[group] & 15) | ((group_minimums[group] >> 4) << 4) | ((group_minimums[group + 4] >> 4) << 6) for group in range(4))
    scale2 = bytes((group_scales[group + 4] & 15) | ((group_minimums[group + 4] & 15) << 4) for group in range(4))
    quants = bytes(codes[(pair * 2) * 32 + half * 16 + lane] | (codes[(pair * 2 + 1) * 32 + half * 16 + lane] << 4) for pair in range(4) for half in range(2) for lane in range(16))
    return struct.pack("<ee", scale, minimum) + scale0 + scale1 + scale2 + quants


def pack_q8_1(scales, values):
    """Pack four logical 32-element Q8_1 groups into one 144-byte record."""
    sums = [scales[group] * sum(values[group * 32 : (group + 1) * 32]) for group in range(4)]
    header = b"".join(struct.pack("<ee", scales[group], sums[group]) for group in range(4))
    quants = bytes(value & 0xFF for group in range(4) for half in range(2) for value in values[group * 32 + half * 16 : group * 32 + (half + 1) * 16])
    return header + quants


def q4k_q8_swiglu(arrays):
    # This is the smallest supported input specialization: two Q4_K blocks and
    # four Q8_1 records. Logical fields are retained separately from their
    # packed bytes so the reference does not repeat the kernel's decode or
    # addressing implementation.
    token_count = 2
    route_count = 8
    route_stride = 11
    expert_count = 128
    tested_expert_count = 2
    output_size = 768
    q4_block_count = 2

    q8_input = bytearray()
    logical_q8 = []
    for token in range(token_count):
        token_groups = []
        for record in range(q4_block_count * 2):
            scales = [2.0 ** -(4 + ((token + record + group) % 3)) for group in range(4)]
            values = [((token * 11 + record * 7 + group * 5 + lane * 3 + lane // 5) % 15) - 7 for group in range(4) for lane in range(32)]
            q8_input.extend(pack_q8_1(scales, values))
            token_groups.append((scales, values))
        logical_q8.append(token_groups)

    logical_weights = {}
    packed_weights = {}
    for name, kind in (("gate", 0), ("up", 1)):
        records = bytearray()
        experts = []
        for expert in range(tested_expert_count):
            rows = []
            for channel in range(output_size):
                blocks = []
                for block in range(q4_block_count):
                    scale = 2.0 ** -(9 + ((expert + channel + block + kind) % 3))
                    minimum = 2.0 ** -(11 + ((expert + channel + 2 * block + kind) % 2))
                    group_scales = [(expert * 17 + channel * 3 + block * 11 + group * 7 + kind * 13) % 64 for group in range(8)]
                    group_minimums = [(expert * 13 + channel * 5 + block * 3 + group * 9 + kind * 7) % 64 for group in range(8)]
                    codes = [(expert * 13 + channel * 7 + block * 5 + group * 3 + lane * 11 + lane // 7 + kind * 9) % 16 for group in range(8) for lane in range(32)]
                    records.extend(pack_q4k(scale, minimum, group_scales, group_minimums, codes))
                    blocks.append((scale, minimum, group_scales, group_minimums, codes))
                rows.append(blocks)
            experts.append(rows)
        logical_weights[name] = experts
        packed_weights[name] = records

    route_ids = []
    for token in range(token_count):
        route_ids.extend((token + route) % tested_expert_count for route in range(route_count))
        route_ids.extend([expert_count - 1] * (route_stride - route_count))
    expected = []
    for token in range(token_count):
        for route in range(route_count):
            expert = route_ids[token * route_stride + route]
            for channel in range(output_size):
                accumulators = {}
                for name in ("gate", "up"):
                    accumulator = 0.0
                    for block in range(q4_block_count):
                        scale, minimum, group_scales, group_minimums, codes = logical_weights[name][expert][channel][block]
                        for group in range(8):
                            q8_scales, q8_values = logical_q8[token][block * 2 + group // 4]
                            q8_scale = q8_scales[group % 4]
                            q8_group = q8_values[(group % 4) * 32 : (group % 4 + 1) * 32]
                            q4_group = codes[group * 32 : (group + 1) * 32]
                            dot = sum(q4 * q8 for q4, q8 in zip(q4_group, q8_group, strict=True))
                            accumulator += q8_scale * scale * group_scales[group] * dot
                            accumulator -= minimum * group_minimums[group] * q8_scale * sum(q8_group)
                    accumulators[name] = accumulator
                gate = accumulators["gate"]
                expected.append(gate / (1.0 + math.exp(-gate)) * accumulators["up"])

    case = Case(arrays, "q4k_q8_swiglu_values", "f32", len(expected))
    case.array("q8_input", [signed_bits(value, 8) for value in q8_input], "i8")
    case.array("route_ids", route_ids, "i32")
    for name in ("gate", "up"):
        case.array(f"{name}_weight", [signed_bits(value, 8) for value in packed_weights[name]], "i8")
    case.scalar("token_count", token_count, "i32")
    case.scalar("route_count", route_count, "i32")
    case.scalar("route_stride", route_stride, "i32")
    case.scalar("expert_count", expert_count, "i32")
    case.scalar("output_size", output_size, "i32")
    case.launch(
        "ffn_routed_gate_up_swiglu_q4k_q8",
        "%token_count, %route_count, %route_stride, %expert_count, %output_size, %q8_input, %route_ids, %gate_weight, %up_weight, %output",
        f"i32, i32, i32, i32, i32, tensor<{len(q8_input)}xi8>, tensor<{token_count * route_stride}xi32>, tensor<{len(packed_weights['gate'])}xi8>, tensor<{len(packed_weights['up'])}xi8>, tensor<{len(expected)}xf32>",
        "%token_count, %route_count, %route_stride, %expert_count, %output_size",
        "i32, i32, i32, i32, i32",
    )
    declaration = "kernel.decl @ffn_routed_gate_up_swiglu_q4k_q8(%workload_token_count: i32, %workload_route_count: i32, %workload_route_stride: i32, %workload_expert_count: i32, %workload_output_size: i32) launch(%token_count: i32, %route_count: i32, %route_stride: i32, %expert_count: i32, %output_size: i32, %q8_input: buffer, %route_ids: buffer, %gate_weight: buffer, %up_weight: buffer, %output: buffer)\n\n"
    return declaration + case.finish(expected, 0.002)


KERNEL_GROUPS = {
    "q4k_q8_swiglu": q4k_q8_swiglu,
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    modes = parser.add_subparsers(required=True)
    kernel = modes.add_parser("kernel", help="emit one kernel check group and its arrays")
    kernel.add_argument("--group", choices=KERNEL_GROUPS, required=True)
    kernel.add_argument("--output", type=Path, required=True)
    kernel.add_argument("--arrays", type=Path, required=True)
    options = parser.parse_args()
    arrays = Arrays(options.arrays, options.output.parent)
    options.output.parent.mkdir(parents=True, exist_ok=True)
    options.output.write_text(KERNEL_GROUPS[options.group](arrays))


if __name__ == "__main__":
    main()
