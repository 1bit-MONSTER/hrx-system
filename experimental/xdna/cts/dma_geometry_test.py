# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Exact native transfers at every compute, memory, and shim DMA wrap limit."""

import argparse
import re
import struct
import subprocess
from pathlib import Path

_GUARD = bytes([0x5A]) * 64
_PROFILES = ("amd.xdna.strix.17f0_10", "amd.xdna.strix_halo.17f0_11")


def _cases():
    # Each row names the physical wrap field width and number of wrap-bearing
    # dimensions. The outermost address dimension has a step but no wrap.
    for tile, bits, dimensions in (
        ("compute", 8, 2),
        ("memory", 10, 3),
        ("shim", 10, 2),
    ):
        for dimension in range(dimensions):
            for accepted in (True, False):
                wrap = (1 << bits) - int(accepted)
                # Singleton inner dimensions select the wrap field under test;
                # non-dense rows prevent collapsing it into a linear transfer.
                strides = [2 * wrap + 16, 1 if dimension == 0 else 2] + [1] * dimension
                for direction in ("ingress", "egress"):
                    yield {
                        "name": f"{tile}_d{dimension}_{wrap}_{direction}",
                        "tile": tile,
                        "dimension": dimension,
                        "wrap": wrap,
                        "accepted": accepted,
                        "direction": direction,
                        "shape": [2, wrap] + [1] * dimension,
                        "strides": strides,
                        "span": 2 * strides[0],
                    }


def _source(case):
    span = case["span"]
    count = 2 * case["wrap"]
    local_span = count if case["tile"] == "shim" else span
    local_row = 1 if case["tile"] == "memory" else 3
    shape = "x".join(map(str, case["shape"])) + "xi32"
    strided = f"{shape}, #encoding.layout.strided<strides={case['strides']}>"
    lines = [
        f"""pipeline.def<kernel> public retain target(@npu) @{case["name"]}() run(%input_arg: buffer, %output_arg: buffer) {{
  %input_unique, %output_unique = buffer.assume.noalias %input_arg, %output_arg : buffer, buffer
  %input, %output = buffer.assume.alignment %input_unique, %output_unique {{minimum_alignment = 4}} : buffer, buffer
  %memory = pipeline.memory<workgroup>[0, {local_row}] : pool
  %zero = index.constant 0 : offset
  %guard = index.constant 64 : offset
  %bytes = index.constant {local_span * 4} : offset
  %capacity = index.constant 1 : index
  %storage = buffer.alloca<workgroup> pool(%memory) align(4) %bytes : buffer
  %slots = buffer.view %storage[%zero] : buffer -> view<1x{local_span}xi32>
  %records = channel.bind<fifo> %slots, %capacity : view<1x{local_span}xi32>, index -> channel<tile<{local_span}xi32>>
  pipeline.strand workers([0, 3], [1, 1], [1, 1]) {{
    %write, %destination = channel.reserve %records : channel<tile<{local_span}xi32>> -> (write<tile<{local_span}xi32>>, view<{local_span}xi32>)"""
    ]

    def copy(name, source_value, target, source_type, target_type, direction):
        lines.extend(
            [
                f"    %{name} = kernel.async.copy %{source_value} to %{target} {{cache_scope = workgroup, cache_temporal = regular, direction = {direction}}} : view<{source_type}> to view<{target_type}> -> kernel.async.token",
                f"    %{name}_group = kernel.async.group %{name} : kernel.async.token -> kernel.async.group",
                f"    kernel.async.wait %{name}_group {{newer_groups = 0}} : kernel.async.group",
            ]
        )

    def project(name, value):
        dimension = case["dimension"]
        full_shape = (
            [2, case["strides"][0]]
            if dimension == 0
            else [2, case["strides"][0] // 2] + [1] * (dimension - 1) + [2]
        )
        full_type = "x".join(map(str, full_shape)) + "xi32"
        refined = f"{full_type}, #encoding.layout.strided<strides={case['strides']}>"
        coordinates = ",".join("0" for _ in full_shape)
        lines.extend(
            [
                f"    %{name}_shaped = view.bitcast %{value} : view<{local_span}xi32> -> view<{full_type}>",
                f"    %{name}_strided = view.refine %{name}_shaped : view<{full_type}> -> view<{refined}>",
                f"    %{name} = view.subview %{name}_strided[{coordinates}] : view<{refined}> -> view<{strided}>",
            ]
        )

    if case["tile"] != "shim":
        lines.append(
            f"    %initial = buffer.view %input[%guard] : buffer -> view<{local_span}xi32>"
        )
        copy(
            "initialize",
            "initial",
            "destination",
            f"{local_span}xi32",
            f"{local_span}xi32",
            "global_to_workgroup",
        )
        project("local_strided", "destination")
        if case["direction"] == "ingress":
            lines.append(
                f"    %incoming = buffer.view %input[%guard] : buffer -> view<{shape}>"
            )
            copy(
                "incoming_copy",
                "incoming",
                "local_strided",
                shape,
                strided,
                "global_to_workgroup",
            )
    else:
        input_type = strided if case["direction"] == "ingress" else f"{count}xi32"
        lines.append(
            f"    %incoming = buffer.view %input[%guard] : buffer -> view<{input_type}>"
        )
        copy(
            "incoming_copy",
            "incoming",
            "destination",
            input_type,
            f"{count}xi32",
            "global_to_workgroup",
        )
    lines.extend(
        [
            f"    channel.publish %write : write<tile<{local_span}xi32>>",
            f"    %read, %payload = channel.acquire %records : channel<tile<{local_span}xi32>> -> (read<tile<{local_span}xi32>>, view<{local_span}xi32>)",
        ]
    )
    if case["tile"] != "shim" and case["direction"] == "egress":
        project("selected", "payload")
        source_value, source_type, output_type = "selected", strided, shape
    else:
        source_value, source_type = "payload", f"{local_span}xi32"
        output_type = (
            strided
            if case["tile"] == "shim" and case["direction"] == "egress"
            else source_type
        )
    lines.append(
        f"    %outgoing = buffer.view %output[%guard] : buffer -> view<{output_type}>"
    )
    copy(
        "outgoing_copy",
        source_value,
        "outgoing",
        source_type,
        output_type,
        "workgroup_to_global",
    )
    lines.extend(
        [f"    channel.release %read : read<tile<{local_span}xi32>>", "  }", "}"]
    )
    return "\n".join(lines)


def _compile(compiler, directory, profile, name, cases):
    path = directory / (name + ".loom")
    image = path.with_suffix(".xdna")
    path.write_text(
        f'aie2p.target<array> @npu {{device_profile = "{profile}"}}\n\n'
        + "\n\n".join(_source(case) for case in cases)
        + "\n"
    )
    result = subprocess.run(
        [compiler, str(path), "--format=xdna", "--output=" + str(image)],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        check=False,
    )
    (directory / (name + ".log")).write_text(result.stdout)
    return image, result


def _pack(values):
    return _GUARD + struct.pack(f"<{len(values)}I", *values) + _GUARD


def _execute(runner, directory, image, case):
    span, count = case["span"], 2 * case["wrap"]
    values = [0x12340000 + index for index in range(span)]
    indices = [
        outer * case["strides"][0] + inner * case["strides"][1]
        for outer in range(2)
        for inner in range(case["wrap"])
    ]
    expected = [0x5A5A5A5A] * span
    if case["tile"] == "shim" and case["direction"] == "egress":
        for index, value in zip(indices, values[:count], strict=True):
            expected[index] = value
    elif case["direction"] == "ingress" and case["tile"] != "shim":
        expected[:] = values
        for index, value in zip(indices, values[:count], strict=True):
            expected[index] = value
    else:
        expected[:count] = [values[index] for index in indices]
    input_bytes = _pack(values)
    (directory / "input.bin").write_bytes(input_bytes)
    (directory / "output.bin").write_bytes(_pack([0x5A5A5A5A] * span))
    command = [
        runner,
        "--columns=1",
        "--entry=" + case["name"],
        "--image=" + str(image),
        "--binding=" + str(directory / "input.bin"),
        "--binding=" + str(directory / "output.bin"),
        "--output=0=" + str(directory / "input-after.bin"),
        "--output=1=" + str(directory / "actual.bin"),
    ]
    subprocess.run(command, check=True)
    # Compare the entire allocations, including both guards and all gaps in a
    # strided destination. No unselected word or read-only input may change.
    if (directory / "input-after.bin").read_bytes() != input_bytes:
        raise AssertionError(f"{case['name']}: read-only input changed")
    if (directory / "actual.bin").read_bytes() != _pack(expected):
        raise AssertionError(f"{case['name']}: transfer, padding, or guard mismatch")
    print(f"{case['name']}: exact transfer, padding, and guards", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--runner", required=True)
    parser.add_argument("--inventory", type=Path, required=True)
    parser.add_argument("--directory", type=Path, required=True)
    arguments = parser.parse_args()
    arguments.directory.mkdir(parents=True, exist_ok=True)
    profiles = re.findall(
        r"^\s+target=(\S+) architecture=\S+$",
        arguments.inventory.read_text(),
        re.MULTILINE,
    )
    # The runner selects XDNA ordinal zero from this same native inventory.
    if not profiles or profiles[0] not in _PROFILES:
        raise AssertionError(f"Expected a supported NPU2 endpoint, got {profiles}")
    cases = list(_cases())
    for profile in _PROFILES:
        directory = arguments.directory / profile
        directory.mkdir()
        image, result = _compile(
            arguments.compiler,
            directory,
            profile,
            "accepted",
            [case for case in cases if case["accepted"]],
        )
        if result.returncode != 0:
            raise AssertionError(
                f"{profile}: accepted geometry failed\n{result.stdout}"
            )
        for case in cases:
            if case["accepted"]:
                continue
            _, result = _compile(
                arguments.compiler, directory, profile, case["name"], [case]
            )
            if (
                result.returncode == 0
                or "XDNA/051" not in result.stdout
                or "an address dimension exceeds the nonzero DMA wrap field"
                not in result.stdout
            ):
                raise AssertionError(
                    f"{profile}/{case['name']}: expected DMA wrap rejection\n{result.stdout}"
                )
        print(
            f"{profile}: all wrap boundaries admitted or rejected correctly", flush=True
        )
        if profile == profiles[0]:
            for case in cases:
                if case["accepted"]:
                    _execute(arguments.runner, directory, image, case)


if __name__ == "__main__":
    main()
