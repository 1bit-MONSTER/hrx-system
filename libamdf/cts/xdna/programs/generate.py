#!/usr/bin/env python3
# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Builds the fixed, finite AIE2P multiply fixtures without runtime dependencies."""

import argparse
import hashlib
import json
import os
import re
import struct
import subprocess
import tempfile
from pathlib import Path

LLVM_REVISION = "3e93bf7b5541b8de37cad32aca9383d90e18c26f"
AIE_REVISION = "8849e208bdcc533b20a0ed3f95c1ce961dee9c3a"
REGISTER_SHA256 = "93fbba403bf30b4a6066afa55ce3102e7f9940aa28df46fdb8c416fada4642d8"
PROFILES = {"Npu4": 0x5354524958000001, "Npu5": 0x535848414C4F0001}
COMPILE_FLAGS = [
    "--target=aie2p-none-unknown-elf",
    "-O2",
    "-ffreestanding",
    "-fno-builtin",
    "-fno-exceptions",
    "-fno-rtti",
    "-fno-vectorize",
    "-fno-slp-vectorize",
    "-fno-ident",
    "-ffunction-sections",
]
LINK_FLAGS = [
    "-Ttext=0",
    "-e",
    "__start",
    "--gc-sections",
    "--defsym=_sp_start_value_DM_stack=0x70400",
]
LICENSE = """// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
"""


def digest(data):
    return hashlib.sha256(data).hexdigest()


def file_digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


class Registers:
    """Exact pinned native definitions; records the constants used by this layout."""

    def __init__(self, path):
        data = path.read_bytes()
        if digest(data) != REGISTER_SHA256:
            raise ValueError("register header does not match the pinned AIE2P source")
        self.values = {
            name: int(value, 0)
            for name, value in re.findall(
                r"^#define XAIE2PGBL_(\w+)\s+(0x[0-9a-fA-F]+|[0-9]+)\s*$",
                data.decode(),
                re.MULTILINE,
            )
        }
        self.used = {}

    def __call__(self, name):
        value = self.values[name]
        self.used[name] = value
        return value

    def field(self, name, value):
        shift, mask = self(name + "_LSB"), self(name + "_MASK")
        assert value >= 0 and (value << shift) & ~mask == 0, name
        return value << shift


class Transaction:
    """The native 0.1 WRITE/BLOCKWRITE/MASKWRITE/MASKPOLL wire layouts."""

    def __init__(self):
        self.data = bytearray(16)
        self.operations = []

    def append(self, kind, row, address, payload, **details):
        offset = len(self.data)
        self.operations.append(
            dict(
                kind=kind,
                row=row,
                address=address,
                byte_offset=offset,
                byte_length=len(payload),
                **details,
            )
        )
        self.data.extend(payload)
        return offset

    def write(self, row, register, value):
        address = (row << 20) + register
        payload = struct.pack("<BBBx4xQII", 0, 0, row, address, value, 24)
        return self.append("write", row, address, payload, value=value)

    def masked(self, kind, row, register, mask, value):
        address = (row << 20) + register
        opcode = {"maskwrite": 3, "maskpoll": 4}[kind]
        payload = struct.pack("<BBBx4xQIII4x", opcode, 0, row, address, value, mask, 32)
        return self.append(kind, row, address, payload, mask=mask, value=value)

    def block(self, row, register, data):
        assert len(data) % 4 == 0
        address = (row << 20) + register
        # The redundant Col/Row members are unused by the native serializer.
        payload = struct.pack("<BBB5xII", 1, 0, row, address, 16 + len(data)) + data
        return (
            self.append(
                "blockwrite", row, address, payload, payload_sha256=digest(data)
            )
            + 16
        )

    def finish(self):
        self.data[:16] = struct.pack(
            "<6BHII", 0, 1, 4, 6, 1, 1, 0, len(self.operations), len(self.data)
        )
        return bytes(self.data)


def build_transaction(registers, text):
    r = registers
    transaction = Transaction()
    core_control = r("CORE_MODULE_CORE_CONTROL")
    reset = r.field("CORE_MODULE_CORE_CONTROL_RESET", 1)
    # This is per-program initialization of already quiescent placement.
    transaction.write(2, core_control, reset)

    channels = [
        (0, "NOC", "S2MM", 0, 2),
        (2, "MEMORY", "S2MM", 0, 0),
        (2, "MEMORY", "S2MM", 1, 1),
        (2, "MEMORY", "MM2S", 0, 2),
        (0, "NOC", "MM2S", 0, 0),
        (0, "NOC", "MM2S", 1, 1),
    ]
    for row, module, direction, channel, _ in channels:
        control = r(f"{module}_MODULE_DMA_{direction}_{channel}_CTRL")
        if module == "MEMORY":
            transaction.write(
                row, control, r.field("MEMORY_MODULE_DMA_S2MM_0_CTRL_RESET", 1)
            )
        transaction.write(row, control, 0)

    # Clear circuit and packet selection on the three routing tiles, then
    # install only the three disjoint circuit paths used by this program.
    for row, module in [(0, "PL"), (1, "MEM_TILE"), (2, "CORE")]:
        for role in ("MASTER", "SLAVE"):
            prefix = f"{module}_MODULE_STREAM_SWITCH_{role}_CONFIG_"
            names = [
                name
                for name in r.values
                if name.startswith(prefix) and name + f"_{role}_ENABLE_MASK" in r.values
            ]
            for name in sorted(names, key=r.values.get):
                transaction.write(row, r(name), 0)

    def route(row, module, slave, master, first_slave):
        prefix = f"{module}_MODULE_STREAM_SWITCH_"
        slave_name = prefix + "SLAVE_CONFIG_" + slave
        master_name = prefix + "MASTER_CONFIG_" + master
        ordinal = (r(slave_name) - r(prefix + "SLAVE_CONFIG_" + first_slave)) // 4
        transaction.write(
            row,
            r(master_name),
            r.field(master_name + "_MASTER_ENABLE", 1)
            | r.field(master_name + "_CONFIGURATION", ordinal),
        )
        transaction.write(row, r(slave_name), r.field(slave_name + "_SLAVE_ENABLE", 1))

    for slave, master in [
        ("SOUTH_3", "NORTH0"),
        ("SOUTH_7", "NORTH1"),
        ("NORTH_0", "SOUTH2"),
    ]:
        route(0, "PL", slave, master, "TILE_CTRL")
    for slave, master in [
        ("SOUTH_0", "NORTH0"),
        ("SOUTH_1", "NORTH1"),
        ("NORTH_0", "SOUTH0"),
    ]:
        route(1, "MEM_TILE", slave, master, "DMA_0")
    for slave, master in [
        ("SOUTH_0", "DMA0"),
        ("SOUTH_1", "DMA1"),
        ("DMA_0", "SOUTH0"),
    ]:
        route(2, "CORE", slave, master, "AIE_CORE0")
    transaction.write(
        0,
        r("NOC_MODULE_MUX_CONFIG"),
        r.field("NOC_MODULE_MUX_CONFIG_SOUTH3", 1)
        | r.field("NOC_MODULE_MUX_CONFIG_SOUTH7", 1),
    )
    transaction.write(
        0, r("NOC_MODULE_DEMUX_CONFIG"), r.field("NOC_MODULE_DEMUX_CONFIG_SOUTH2", 1)
    )

    transaction.block(2, r("CORE_MODULE_PROGRAM_MEMORY"), text)
    for lock, initial in enumerate([1, 0, 1, 0, 1, 0]):
        transaction.write(2, r(f"MEMORY_MODULE_LOCK{lock}_VALUE"), initial)

    for descriptor, (offset, acquire, release) in enumerate(
        [(0x2000, 0, 1), (0x2100, 2, 3), (0x2200, 5, 4)]
    ):
        prefix = "MEMORY_MODULE_DMA_BD0_"
        words = [0] * 6
        words[0] = r.field(prefix + "0_BASE_ADDRESS", offset // 4) | r.field(
            prefix + "0_BUFFER_LENGTH", 16
        )
        # Zero steps encode a one-word step. Zero wraps select linear DMA.
        # The signed seven-bit acquire value -1 consumes one available credit.
        words[5] = (
            r.field(prefix + "5_VALID_BD", 1)
            | r.field(prefix + "5_LOCK_ACQ_ENABLE", 1)
            | r.field(prefix + "5_LOCK_ACQ_ID", acquire)
            | r.field(prefix + "5_LOCK_ACQ_VALUE", 127)
            | r.field(prefix + "5_LOCK_REL_ID", release)
            | r.field(prefix + "5_LOCK_REL_VALUE", 1)
        )
        transaction.block(
            2, r(f"MEMORY_MODULE_DMA_BD{descriptor}_0"), struct.pack("<6I", *words)
        )

    relocations = []
    for descriptor in range(3):
        prefix = "NOC_MODULE_DMA_BD0_"
        words = [0] * 8
        words[0] = r.field(prefix + "0_BUFFER_LENGTH", 16)
        words[4] = r.field(prefix + "4_BURST_LENGTH", 3)
        words[5] = r.field(prefix + "5_AXCACHE", 2)
        words[7] = r.field(prefix + "7_VALID_BD", 1)
        payload_offset = transaction.block(
            0, r(f"NOC_MODULE_DMA_BD{descriptor}_0"), struct.pack("<8I", *words)
        )
        relocations.append(payload_offset + 4)

    # One task, no next BD, no token. Each channel has exactly one fixed BD.
    # Sinks are queued before sources; the input sources unblock the core.
    for row, module, direction, channel, descriptor in channels:
        suffix = "TASK_QUEUE" if module == "NOC" else "START_QUEUE"
        name = f"{module}_MODULE_DMA_{direction}_{channel}_{suffix}"
        transaction.write(row, r(name), r.field(name + "_START_BD_ID", descriptor))
    transaction.write(2, core_control, 0)
    transaction.write(2, core_control, r.field("CORE_MODULE_CORE_CONTROL_ENABLE", 1))

    # Enable clears the previous DONE generation. These are native ordinary
    # waits, not timeouts or cancellation. Every started actor has a final join.
    done_mask = r("CORE_MODULE_CORE_STATUS_CORE_DONE_MASK")
    transaction.masked(
        "maskpoll", 2, r("CORE_MODULE_CORE_STATUS"), done_mask, done_mask
    )
    for row, module, direction, channel, _ in channels:
        # The driver's common WaitForDone uses the S2MM definitions for both
        # directions: the MM2S stream/TCT bits have the same positions.
        prefix = f"{module}_MODULE_DMA_S2MM_STATUS_0_"
        mask = 0
        for field in [
            "TASK_QUEUE_SIZE",
            "CHANNEL_RUNNING",
            "STALLED_LOCK_ACQ",
            "STALLED_LOCK_REL",
            "STALLED_STREAM_STARVATION",
            "STALLED_TCT_OR_COUNT_FIFO_FULL",
        ]:
            mask |= r(prefix + field + "_MASK")
        transaction.masked(
            "maskpoll",
            row,
            r(f"{module}_MODULE_DMA_{direction}_STATUS_{channel}"),
            mask,
            0,
        )
    return transaction.finish(), relocations, transaction.operations


def executable(profile, transaction, relocations):
    """One mutable COMMAND allocation, three GLOBAL bindings, one invocation."""
    name = b"mul_i32"
    metadata = struct.pack(
        "<IHHIIQQHH7I",
        0x414E4458,
        2,
        1,
        3,
        1,
        profile,
        0x4E5055320006000C,
        1,
        6,
        1,
        1,
        1,
        3,
        3,
        1,
        len(name),
    )
    metadata += struct.pack("<IIQQII", 1, 0, len(transaction), 32768, 1, 1)
    metadata += struct.pack("<I", 0)
    metadata += struct.pack("<12I", 0, len(name), 0, 1, 0, 3, 0, 0, 0, 3, 0, 1)
    for access in [1, 1, 2]:
        metadata += struct.pack("<4H4Q", 1, 1, access, 5, 64, 4, 0, 0)
    for binding, offset in enumerate(relocations):
        metadata += struct.pack(
            "<4Iq3Q", 0, binding, offset, 1, 0, 0, (1 << 48) - 64, 4
        )
    metadata += struct.pack("<4I", 0, 0, len(transaction), 0) + name
    payload_offset = (116 + len(metadata) + 3) & ~3
    header = struct.pack(
        "<16sHH5I6H",
        b"\x7fELF\x01\x01\x01" + bytes(9),
        2,
        264,
        1,
        0,
        52,
        0,
        3,
        52,
        32,
        2,
        0,
        0,
        0,
    )
    header += struct.pack(
        "<8I", 0x6C584408, 116, 0, 0, len(metadata), len(metadata), 4, 4
    )
    header += struct.pack(
        "<8I", 1, payload_offset, 0, 0, len(transaction), len(transaction), 4, 4
    )
    return header + metadata + bytes(payload_offset - 116 - len(metadata)) + transaction


def extract_text(elf):
    assert elf[:7] == b"\x7fELF\x01\x01\x01"
    assert struct.unpack_from("<H", elf, 18)[0] == 264
    assert struct.unpack_from("<I", elf, 24)[0] == 0
    assert struct.unpack_from("<I", elf, 36)[0] == 3
    section_offset = struct.unpack_from("<I", elf, 32)[0]
    section_size, section_count, name_index = struct.unpack_from("<3H", elf, 46)
    sections = [
        struct.unpack_from("<10I", elf, section_offset + i * section_size)
        for i in range(section_count)
    ]
    names = elf[sections[name_index][4] :][: sections[name_index][5]]
    text = None
    for section in sections:
        name = names[section[0] :].split(b"\0", 1)[0]
        assert section[1] not in (4, 9) or section[5] == 0, "linked relocation"
        if section[2] & 2 and section[5]:
            assert name == b".text" and section[1] == 1 and section[3] == 0
            text = elf[section[4] :][: section[5]]
        if section[1] == 2:
            for offset in range(section[4] + 16, section[4] + section[5], 16):
                assert struct.unpack_from("<H", elf, offset + 14)[0] != 0
    assert text and len(text) % 4 == 0 and len(text) <= 16384
    return text


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--toolchain", required=True, type=Path)
    parser.add_argument("--registers", required=True, type=Path)
    parser.add_argument("--evidence-dir", type=Path)
    parser.add_argument("--check", action="store_true")
    arguments = parser.parse_args()
    directory = Path(__file__).resolve().parent
    toolchain = arguments.toolchain.resolve()
    environment = dict(os.environ, LD_LIBRARY_PATH=str(toolchain / "lib"))

    def run(tool, *args, cwd=None):
        return subprocess.check_output(
            [str(toolchain / "bin" / tool), *map(str, args)], cwd=cwd, env=environment
        )

    version = (
        "\n".join(
            line
            for line in run("clang++", "--version").decode().splitlines()
            if not line.startswith("InstalledDir:")
        )
        + "\n"
    )
    if LLVM_REVISION not in version:
        raise ValueError("LLVM-AIE does not match the pinned compiler revision")
    registers = Registers(arguments.registers)
    source = directory / "mul_i32.cc"
    with tempfile.TemporaryDirectory(prefix="xdna-mul-") as temporary:
        temporary = Path(temporary)
        (temporary / source.name).write_bytes(source.read_bytes())
        run(
            "clang++",
            *COMPILE_FLAGS,
            "-MD",
            "-MF",
            "dependencies.d",
            "-c",
            source.name,
            "-o",
            "mul_i32.o",
            cwd=temporary,
        )
        runtime = toolchain / "lib/aie2p-none-unknown-elf"
        run(
            "ld.lld",
            *LINK_FLAGS,
            runtime / "crt0.o",
            runtime / "crt1.o",
            "mul_i32.o",
            "-o",
            "mul_i32.elf",
            cwd=temporary,
        )
        elf = (temporary / "mul_i32.elf").read_bytes()
        text = extract_text(elf)
        disassembly = run(
            "llvm-objdump", "-d", "--disassemble-zeroes", "mul_i32.elf", cwd=temporary
        )
        inspection = run(
            "llvm-readelf", "-h", "-S", "-r", "-s", "mul_i32.elf", cwd=temporary
        )
        dependencies = (
            (temporary / "dependencies.d").read_text().replace("\\\n", "").split()[2:]
        )
        header_inputs = {
            str(Path(path).relative_to(toolchain)): file_digest(Path(path))
            for path in dependencies
            if Path(path).is_absolute()
        }

    transaction, relocations, operations = build_transaction(registers, text)
    images = {
        name: executable(profile, transaction, relocations)
        for name, profile in PROFILES.items()
    }
    header = (
        LICENSE
        + """
// Generated by generate.py. Each ELF contains the complete finite program.
#ifndef AMDF_CTS_XDNA_PROGRAMS_MUL_I32_H_
#define AMDF_CTS_XDNA_PROGRAMS_MUL_I32_H_

#include <array>
#include <cstdint>

namespace amdf::cts::xdna::programs {

// clang-format off
"""
    )
    for name, image in images.items():
        header += f"\ninline constexpr std::array<uint8_t, {len(image)}> kMulI32{name}Elf = {{\n"
        for offset in range(0, len(image), 12):
            header += (
                "    "
                + ", ".join(f"0x{value:02x}" for value in image[offset : offset + 12])
                + ",\n"
            )
        header += "};\n"
    header += "\n// clang-format on\n\n}  // namespace amdf::cts::xdna::programs\n\n#endif  // AMDF_CTS_XDNA_PROGRAMS_MUL_I32_H_\n"
    tool_inputs = [
        "bin/clang",
        "bin/ld.lld",
        "bin/llvm-objdump",
        "bin/llvm-readelf",
        "lib/libclang-cpp.so",
        "lib/libLLVM.so",
        "lib/aie2p-none-unknown-elf/crt0.o",
        "lib/aie2p-none-unknown-elf/crt1.o",
    ]
    manifest = {
        "schema_version": 1,
        "llvm_aie_revision": LLVM_REVISION,
        "aie_rt_revision": AIE_REVISION,
        "register_header": "driver/src/global/xaie2pgbl_params.h",
        "register_header_sha256": REGISTER_SHA256,
        "source_sha256": file_digest(source),
        "generator_sha256": file_digest(Path(__file__)),
        "compiler_version": version,
        "compile_flags": COMPILE_FLAGS,
        "link_flags": LINK_FLAGS,
        "toolchain_inputs_sha256": {
            path: file_digest(toolchain / path) for path in tool_inputs
        },
        "compiler_headers_sha256": header_inputs,
        "worker_elf_sha256": digest(elf),
        "worker_text_byte_length": len(text),
        "worker_text_sha256": digest(text),
        "worker_disassembly": disassembly.decode(),
        "transaction_byte_length": len(transaction),
        "transaction_sha256": digest(transaction),
        "dynamic_relocation_byte_offsets": relocations,
        "images": {
            name: dict(
                profile=f"0x{PROFILES[name]:016x}",
                byte_length=len(image),
                sha256=digest(image),
            )
            for name, image in images.items()
        },
        "register_constants": dict(sorted(registers.used.items())),
        "operations": operations,
    }
    outputs = {
        "mul_i32.h": header.encode(),
        "mul_i32.json": (
            json.dumps(manifest, indent=2, sort_keys=True) + "\n"
        ).encode(),
    }
    for name, data in outputs.items():
        path = directory / name
        if arguments.check:
            if path.read_bytes() != data:
                raise ValueError(f"generated output differs: {name}")
        else:
            path.write_bytes(data)
    if arguments.evidence_dir:
        arguments.evidence_dir.mkdir(parents=True, exist_ok=True)
        evidence = {
            "mul_i32.elf": elf,
            "mul_i32.text.bin": text,
            "transaction.bin": transaction,
            "disassembly.txt": disassembly,
            "inspection.txt": inspection,
        }
        evidence.update(
            {f"mul_i32_{name.lower()}.xdna": image for name, image in images.items()}
        )
        for name, data in evidence.items():
            (arguments.evidence_dir / name).write_bytes(data)
    print(
        json.dumps(
            {
                "checked": arguments.check,
                "images": manifest["images"],
                "transaction_byte_length": len(transaction),
            },
            indent=2,
        )
    )


if __name__ == "__main__":
    main()
