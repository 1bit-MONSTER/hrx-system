# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Exact subgroup payload transport, including NaN bits and packed tails."""

import argparse
import struct
from pathlib import Path

from loom.gen.test.kernel_fixture import Arrays, Case, signed_bits


def transport_cases(arrays, element, width, count):
    # Repeat every 32 invocations so the reference applies to either physical
    # wave size, including the partial wave64 tail of the 96-invocation launch.
    special = {
        16: [0, 0x8000, 1, 0x8001, 0x7C00, 0x7C01, 0x7E01, 0x7F80, 0x7F81, 0x7FC1, 0xFFFF],
        64: [0, 1 << 63, 1, (1 << 63) | 1, 0x7FF0000000000000, 0x7FF0000000000001, 0xFFF8123456789ABC],
    }
    values = []
    for item in range(96):
        for component in range(count):
            ordinal = (item % 32) * count + component
            bits = ordinal * 0x123456789ABCDEF + 0xFEDCBA9876543210
            if width in special and ordinal < len(special[width]):
                bits = special[width][ordinal]
            values.append(signed_bits(bits, width))
    storage = f"i{width}"
    kernel = f"subgroup_transport_{element.lower()}_{count}"
    results = [f"kernel.decl @{kernel}() launch(%selected: index, %input: buffer, %output: buffer)\n"]
    for selected in range(32):
        case = Case(arrays, f"{kernel}_lane_{selected}", storage, 288 * count)
        case.array("input", values)
        case.scalar("selected", selected, "index")
        case.launch(kernel, "%selected, %input, %output", f"index, tensor<{96 * count}x{storage}>, tensor<{288 * count}x{storage}>")
        named = values[selected * count : (selected + 1) * count] * 96
        first = values[5 * count : 6 * count]
        divergent = [value for item in range(96) for value in (first if 5 <= item % 32 < 10 else [-123] * count)]
        results.append(case.finish(named + named + divergent))
    return results


def uniform_case(arrays):
    # Signed zeros exercise raw floating argument transport without host NaN
    # canonicalization. The loaded cases above supply quiet/signaling NaNs.
    arguments = [
        ("i8", 8, -91),
        ("i16", 16, -23101),
        ("i32", 32, -19088743),
        ("i64", 64, -81985529216486896),
        ("f8E4M3", 8, "-0.0"),
        ("f8E5M2", 8, "-0.0"),
        ("f16", 16, "-0.0"),
        ("bf16", 16, "-0.0"),
        ("f32", 32, "-0.0"),
        ("f64", 64, "-0.0"),
    ]
    case = Case(arrays, "subgroup_transport_uniform_bits", "i8", 32 * 80)
    expected = [-123] * 80
    for ordinal, (element, width, value) in enumerate(arguments):
        case.scalar(f"value_{ordinal}", value, element)
        bits = (1 << (width - 1)) if isinstance(value, str) else value % (1 << width)
        expected[8 * ordinal : 8 * ordinal + width // 8] = [signed_bits(byte, 8) for byte in bits.to_bytes(width // 8, "little")]
    case.launch("subgroup_transport_uniform", ", ".join(f"%value_{i}" for i in range(len(arguments))) + ", %output", ", ".join(element for element, _, _ in arguments) + ", tensor<2560xi8>")
    declaration = "kernel.decl @subgroup_transport_uniform() launch(" + ", ".join(f"%value_{i}: {element}" for i, (element, _, _) in enumerate(arguments)) + ", %output: buffer)\n\n"
    return declaration + case.finish(expected * 32)


def carrier_cases(arrays, width):
    kernel = f"subgroup_carrier_i{width}"
    inputs = [signed_bits(0xDEADBEEF + item * 0x1234567, 32) for item in range(32)]
    results = [f"kernel.decl @{kernel}() launch(%selected: index, %input: buffer, %output: buffer)\n"]
    for selected in range(32):
        case = Case(arrays, f"{kernel}_lane_{selected}", "i32", 128)
        case.array("input", inputs)
        case.scalar("selected", selected, "index")
        case.launch(kernel, "%selected, %input, %output", "index, tensor<32xi32>, tensor<128xi32>")
        signed = signed_bits(inputs[selected], width)
        unsigned = inputs[selected] % (1 << width)
        signed_float = struct.unpack("<i", struct.pack("<f", signed))[0]
        unsigned_float = struct.unpack("<i", struct.pack("<f", unsigned))[0]
        results.append(case.finish([signed] * 32 + [unsigned] * 32 + [signed_float] * 32 + [unsigned_float] * 32))
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--arrays", type=Path, required=True)
    args = parser.parse_args()
    arrays = Arrays(args.arrays, args.output.parent)
    cases = []
    for element, width, count in [("i64", 64, 1), ("i8", 8, 7), ("f16", 16, 3), ("bf16", 16, 4), ("f8E4M3", 8, 8), ("f8E5M2", 8, 8), ("f64", 64, 2)]:
        cases.extend(transport_cases(arrays, element, width, count))
    cases.append(uniform_case(arrays))
    for width in [8, 16]:
        cases.extend(carrier_cases(arrays, width))
    args.output.write_text("\n".join(cases))


if __name__ == "__main__":
    main()
