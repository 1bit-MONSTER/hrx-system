#!/usr/bin/env python3
# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 WITH LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Regenerates fixed CTS images with their pinned offline compiler."""

import argparse
import hashlib
import json
import re
import shutil
import struct
import subprocess
import tempfile
from pathlib import Path

LLVM_REVISION = "6dfe1677ab8dffbc6ec13d53a1e0215d75147689"
COMMON_COMPILE_FLAGS = [
    "-mcode-object-version=5",
    "-mllvm",
    "-amdgpu-kernarg-preload=false",
    "-nogpulib",
    "-std=c23",
    "-O2",
    "-fno-ident",
]
LINK_FLAGS = ["-shared", "--no-undefined", "--build-id=none", "--no-rosegment"]
EXTRACT_FLAGS = ["--only-section=.rodata", "--only-section=.text", "-O", "binary"]
TARGETS = {
    "gfx942": {
        "compile_flags": [],
        "elf_flags": 0x54C,
        "wavefront_size": 64,
        "kernel_code_properties": 8,
    },
    "gfx1151": {
        "compile_flags": ["-mno-wavefrontsize64"],
        "elf_flags": 0x4A,
        "wavefront_size": 32,
        "kernel_code_properties": 0x408,
    },
}
FIXTURES = {
    "byte_copy_unaligned": {
        "targets": ["gfx942"],
        "symbol": "byte_copy_unaligned",
        "group_byte_length": 0,
        "private_byte_length": 0,
        "kernarg_byte_length": 36,
        "workgroup_size": 64,
        "arguments": [
            ("0", "8", "global_buffer"),
            ("8", "8", "global_buffer"),
            ("16", "8", "by_value"),
            ("24", "4", "by_value"),
            ("28", "4", "by_value"),
            ("32", "4", "by_value"),
        ],
    },
    "transform": {
        "targets": ["gfx942", "gfx1151"],
        "gfx1151_resource_words": (0x20, 0xE0AF0000, 0x84),
        "symbol": "aql_transform",
        "group_byte_length": 0,
        "private_byte_length": 0,
        "kernarg_byte_length": 24,
        "workgroup_size": 64,
        "arguments": [
            ("0", "8", "global_buffer"),
            ("8", "8", "global_buffer"),
            ("16", "4", "by_value"),
            ("20", "4", "by_value"),
        ],
    },
    "private_roundtrip": {
        "targets": ["gfx942"],
        "symbol": "private_roundtrip",
        "group_byte_length": 0,
        "private_byte_length": 40,
        "kernarg_byte_length": 16,
        "workgroup_size": 64,
        "arguments": [
            ("0", "8", "global_buffer"),
            ("8", "4", "by_value"),
            ("12", "4", "by_value"),
        ],
    },
    "lds_exchange": {
        "targets": ["gfx942", "gfx1151"],
        "gfx1151_resource_words": (0x30, 0xE0AF0000, 0x84),
        "symbol": "lds_exchange",
        "group_byte_length": 512,
        "private_byte_length": 0,
        "kernarg_byte_length": 20,
        "workgroup_size": 128,
        "arguments": [
            ("0", "8", "global_buffer"),
            ("8", "4", "dynamic_shared_pointer"),
            ("12", "4", "by_value"),
            ("16", "4", "by_value"),
        ],
    },
    "geometry_ids": {
        "targets": ["gfx942"],
        "symbol": "geometry_ids",
        "group_byte_length": 0,
        "private_byte_length": 0,
        "kernarg_byte_length": 32,
        "workgroup_size": 64,
        "arguments": [
            ("0", "8", "global_buffer"),
            ("8", "4", "by_value"),
            ("12", "4", "by_value"),
            ("16", "4", "by_value"),
            ("20", "4", "by_value"),
            ("24", "4", "by_value"),
            ("28", "4", "by_value"),
        ],
    },
}
COPYRIGHT = """// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
"""


def require(condition, message):
    if not condition:
        raise ValueError(message)


def inspect_image(elf, image, notes, fixture, target_name):
    # This parser inspects fixed compiler-produced fixtures during generation.
    # It is not linked into the CTS or a production code-object loader.
    header = struct.unpack_from("<16sHHIQQQIHHHHHH", elf)
    require(header[0][:9] == b"\x7fELF\x02\x01\x01\x40\x03", "expected HSA V5 ELF64")
    require(header[1:4] == (3, 224, 1), "expected AMDGPU shared ELF")
    target = TARGETS[target_name]
    require(header[7] == target["elf_flags"], "unexpected ELF target/features")
    require(header[11] == 64, "unexpected ELF section header size")
    sections = [
        struct.unpack_from("<IIQQQQIIQQ", elf, header[6] + index * 64)
        for index in range(header[12])
    ]
    names_section = sections[header[13]]
    names = elf[names_section[4] : names_section[4] + names_section[5]]

    def section_name(section):
        return names[section[0] :].split(b"\0", 1)[0].decode()

    by_name = {section_name(section): section for section in sections}
    for section in sections:
        require(section[1] not in (4, 9) or section[5] == 0, "unexpected relocation")
    for name in (".data", ".bss", ".got", ".plt", ".init_array", ".fini_array"):
        require(name not in by_name or by_name[name][5] == 0, f"unexpected {name}")

    symbols_section = by_name[".dynsym"]
    strings_section = sections[symbols_section[6]]
    strings = elf[strings_section[4] : strings_section[4] + strings_section[5]]
    require(symbols_section[9] == 24, "unexpected ELF symbol size")
    symbols = {}
    for offset in range(
        symbols_section[4], symbols_section[4] + symbols_section[5], 24
    ):
        symbol = struct.unpack_from("<IBBHQQ", elf, offset)
        name = strings[symbol[0] :].split(b"\0", 1)[0].decode()
        if name:
            require(symbol[3] != 0, f"undefined symbol: {name}")
            symbols[name] = symbol
    symbol_name = fixture["symbol"]
    require(
        set(symbols) == {symbol_name, symbol_name + ".kd"},
        "unexpected exported symbols",
    )
    descriptor_symbol = symbols[symbol_name + ".kd"]
    entry_symbol = symbols[symbol_name]
    descriptor_section = by_name[".rodata"]
    text_section = by_name[".text"]
    require(
        descriptor_symbol[4] == descriptor_section[3]
        and descriptor_symbol[5] == descriptor_section[5] == 64,
        "rodata must contain exactly one descriptor",
    )
    require(
        entry_symbol[4] == text_section[3] and 0 < entry_symbol[5] <= text_section[5],
        "text must begin with the kernel entry",
    )
    image_base = descriptor_section[3]
    entry_offset = text_section[3] - image_base
    require(
        image_base % 64 == 0 and text_section[3] % 256 == 0,
        "descriptor/entry alignment",
    )
    require(entry_offset >= 64, "overlapping descriptor and text")
    expected_image = bytearray(entry_offset + text_section[5])
    for section in (descriptor_section, text_section):
        offset = section[3] - image_base
        expected_image[offset : offset + section[5]] = elf[
            section[4] : section[4] + section[5]
        ]
    require(image == expected_image, "extraction changed descriptor/text layout")
    require(
        struct.unpack_from("<III", image)
        == (
            fixture["group_byte_length"],
            fixture["private_byte_length"],
            fixture["kernarg_byte_length"],
        ),
        "segment requirements",
    )
    require(struct.unpack_from("<q", image, 16)[0] == entry_offset, "entry offset")
    require(image[12:16] == bytes(4), "descriptor reserved0")
    require(image[24:44] == bytes(20), "descriptor reserved1")
    require(image[60:64] == bytes(4), "descriptor reserved3")
    require(
        struct.unpack_from("<HH", image, 56) == (target["kernel_code_properties"], 0),
        "kernarg input/wave mode/preload",
    )
    if target_name == "gfx1151":
        # The PM4 caller preserves these exact compiler words while deriving
        # the LDS allocation separately. Each fixture's input and prefetch
        # contracts remain explicit.
        require(
            struct.unpack_from("<III", image, 44) == fixture["gfx1151_resource_words"],
            "unexpected gfx1151 program resources",
        )

    arguments = re.findall(
        r"\.offset:\s+(\d+)\s+(?:\.pointee_align:\s+\d+\s+)?"
        r"\.size:\s+(\d+)\s+\.value_kind:\s+(\w+)",
        notes,
    )
    require(
        arguments == fixture["arguments"],
        "unexpected kernel argument metadata",
    )

    def metadata_value(name):
        values = re.findall(r"\." + re.escape(name) + r":\s+(\S+)", notes)
        require(len(values) == 1, f"expected one metadata field: {name}")
        return values[0]

    expected_metadata = {
        "group_segment_fixed_size": str(fixture["group_byte_length"]),
        "private_segment_fixed_size": str(fixture["private_byte_length"]),
        "kernarg_segment_size": str(fixture["kernarg_byte_length"]),
        "kernarg_segment_align": "8",
        "max_flat_workgroup_size": str(fixture["workgroup_size"]),
        "wavefront_size": str(target["wavefront_size"]),
        "sgpr_spill_count": "0",
        "vgpr_spill_count": "0",
        "uses_dynamic_stack": "false",
    }
    for name, expected in expected_metadata.items():
        require(metadata_value(name) == expected, f"unexpected {name}")
    # Retain the linked address phase at a 256-byte-aligned allocation base.
    # The descriptor itself is only 64-byte aligned; its relative entry offset
    # remains unchanged. Prefix bytes are placement padding, not relocations.
    descriptor_offset = image_base % 256
    metadata = {
        "descriptor_byte_offset": descriptor_offset,
        "entry_byte_offset": descriptor_offset + entry_offset,
        "entry_byte_length": entry_symbol[5],
        "text_byte_length": text_section[5],
        "group_segment_byte_length": fixture["group_byte_length"],
        "private_segment_byte_length": fixture["private_byte_length"],
        "kernarg_byte_length": fixture["kernarg_byte_length"],
        "kernarg_metadata_alignment": 8,
        "kernarg_allocation_alignment": 16,
        "workgroup_size": fixture["workgroup_size"],
        "wavefront_size": target["wavefront_size"],
        "sgpr_count": int(metadata_value("sgpr_count")),
        "vgpr_count": int(metadata_value("vgpr_count")),
        "agpr_count": int(metadata_value("agpr_count"))
        if target_name == "gfx942"
        else 0,
        "compute_pgm_rsrc3": struct.unpack_from("<I", image, 44)[0],
        "compute_pgm_rsrc1": struct.unpack_from("<I", image, 48)[0],
        "compute_pgm_rsrc2": struct.unpack_from("<I", image, 52)[0],
        "kernel_code_properties": target["kernel_code_properties"],
        "kernarg_preload": 0,
        "relocations": [],
    }
    return bytes(descriptor_offset) + image, metadata, entry_symbol[4]


def render_header(name, target_name, image, metadata):
    words = struct.unpack(f"<{len(image) // 4}I", image)
    rows = [
        "    " + ", ".join(f"0x{word:08x}u" for word in words[index : index + 5]) + ","
        for index in range(0, len(words), 5)
    ]
    resource_constants = ""
    if target_name == "gfx1151":
        resource_constants = "".join(
            f"inline constexpr uint32_t kComputePgmRsrc{index} = "
            f"0x{metadata[f'compute_pgm_rsrc{index}']:08x}u;\n"
            for index in (1, 2, 3)
        )
    return (
        COPYRIGHT
        + f"""
// Generated by generate.py; see {name}_{target_name}.json.
// Compiler descriptor and code remain paired in this little-endian image.

#ifndef AMDF_CTS_GPU_KERNELS_{name.upper()}_{target_name.upper()}_H_
#define AMDF_CTS_GPU_KERNELS_{name.upper()}_{target_name.upper()}_H_

#include <array>
#include <cstdint>

#include "libamdf/cts/gpu/kernels/image.h"

namespace kernels::{target_name}_{name} {{

inline constexpr char kImageSha256[] =
    "{hashlib.sha256(image).hexdigest()}";
inline constexpr uint32_t kDescriptorByteOffset = {metadata["descriptor_byte_offset"]};
inline constexpr uint32_t kEntryByteOffset = {metadata["entry_byte_offset"]};
inline constexpr uint32_t kKernargByteLength = {metadata["kernarg_byte_length"]};
inline constexpr uint32_t kKernargAlignment = 16;
inline constexpr uint32_t kGroupSegmentByteLength = {metadata["group_segment_byte_length"]};
inline constexpr uint32_t kPrivateSegmentByteLength = {metadata["private_segment_byte_length"]};
inline constexpr uint16_t kWorkgroupSize = {metadata["workgroup_size"]};
{resource_constants}
alignas(256) inline constexpr std::array<uint32_t, {len(words)}> kImage = {{
{chr(10).join(rows)}
}};

inline constexpr Image kExecutable = {{
    kImage.data(),
    sizeof(kImage),
    kDescriptorByteOffset,
    kImageSha256,
}};

}}  // namespace kernels::{target_name}_{name}

#endif  // AMDF_CTS_GPU_KERNELS_{name.upper()}_{target_name.upper()}_H_
"""
    )


def generate_fixture(
    name, fixture, target_name, source_dir, compiler, linker, run, check
):
    compile_flags = [
        "--target=amdgcn-amd-amdhsa",
        f"-mcpu={target_name}",
        *TARGETS[target_name]["compile_flags"],
        *COMMON_COMPILE_FLAGS,
    ]
    with tempfile.TemporaryDirectory(
        prefix=f"amdf-{target_name}-fixture-"
    ) as temporary:
        work = Path(temporary)
        run(
            "clang",
            [
                *compile_flags,
                "-c",
                str(source_dir / f"{name}.c"),
                "-o",
                f"{name}.o",
            ],
            work,
        )
        run("ld.lld", [*LINK_FLAGS, f"{name}.o", "-o", f"{name}.hsaco"], work)
        run("llvm-objcopy", [*EXTRACT_FLAGS, f"{name}.hsaco", f"{name}.bin"], work)
        elf = (work / f"{name}.hsaco").read_bytes()
        image = (work / f"{name}.bin").read_bytes()
        notes = run("llvm-readelf", ["--notes", f"{name}.hsaco"], work)
        image, metadata, entry_address = inspect_image(
            elf, image, notes, fixture, target_name
        )
        disassembly = run(
            "llvm-objdump",
            [
                "--disassemble",
                f"--mcpu={target_name}",
                f"--start-address={entry_address}",
                f"--stop-address={entry_address + metadata['entry_byte_length']}",
                f"{name}.hsaco",
            ],
            work,
        )
    record = {
        "source": f"{name}.c",
        "source_sha256": hashlib.sha256(
            (source_dir / f"{name}.c").read_bytes()
        ).hexdigest(),
        "llvm_revision": LLVM_REVISION,
        "compiler": compiler,
        "linker": linker,
        "compile_flags": compile_flags,
        "link_flags": LINK_FLAGS,
        "extract_flags": EXTRACT_FLAGS,
        "target": target_name,
        **({"xnack": "any", "sramecc": "any"} if target_name == "gfx942" else {}),
        "code_object_version": 5,
        "elf_sha256": hashlib.sha256(elf).hexdigest(),
        "image_sha256": hashlib.sha256(image).hexdigest(),
        "image_byte_length": len(image),
        "kernel": metadata,
        "compiler_metadata": notes.strip(),
        "disassembly": disassembly.strip(),
    }
    outputs = {
        f"{name}_{target_name}.h": render_header(name, target_name, image, metadata),
        f"{name}_{target_name}.json": json.dumps(record, indent=2) + "\n",
    }
    for output_name, text in outputs.items():
        path = source_dir / output_name
        if check:
            require(
                path.read_text() == text,
                f"{output_name} differs; regenerate with generate.py",
            )
        else:
            path.write_text(text)
    print(
        f"{'Verified' if check else 'Generated'} {name} {target_name} image: "
        f"{len(image)} bytes, {record['image_sha256']}"
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--llvm-bin", type=Path, help="directory of the pinned LLVM tools"
    )
    parser.add_argument("--check", action="store_true", help="verify without rewriting")
    args = parser.parse_args()
    source_dir = Path(__file__).resolve().parent
    if args.llvm_bin is None:
        clang = shutil.which("clang")
        require(clang is not None, "clang not found; pass --llvm-bin")
        args.llvm_bin = Path(clang).resolve().parent
    tools = args.llvm_bin.resolve()

    def run(tool, arguments, directory):
        return subprocess.check_output(
            [str(tools / tool), *arguments], cwd=directory, text=True
        )

    compiler = run("clang", ["--version"], source_dir).splitlines()[0]
    linker = run("ld.lld", ["--version"], source_dir).splitlines()[0]
    require(
        LLVM_REVISION in compiler and LLVM_REVISION in linker, "LLVM revision mismatch"
    )
    for name, fixture in FIXTURES.items():
        for target_name in fixture["targets"]:
            generate_fixture(
                name,
                fixture,
                target_name,
                source_dir,
                compiler,
                linker,
                run,
                args.check,
            )


if __name__ == "__main__":
    main()
