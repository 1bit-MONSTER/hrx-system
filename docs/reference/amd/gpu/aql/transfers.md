# AQL-carried memory operations

AMD vendor-format-1 AQL packets carry a PM4 indirect buffer on a native AQL
queue. ROCr uses this route on GFX9 and newer targets for command-processor
operations, including globally visible memory operations. The carrier borrows
command storage through execution completion; consuming its ring slot is a
different event. [ROCr carrier][rocr-queue] · [Memory-operation
caller][rocr-data]

## Carrier representation and publication

The following is the format emitted by ROCr's GFX9-and-newer `ExecutePM4`
branch. Its older branch through GFX8 has a different implementation and
completion path.

| Packet DWORD | Meaning |
| --- | --- |
| 0 | Vendor type 0 in bits 7:0; standard barrier bit 8 and acquire/release fields at bits 9/11; AMD format 1 in bits 23:16. |
| 1 | Four-DWORD INDIRECT_BUFFER header, `0xc0023f00` on GFX9. |
| 2–3 | DWORD-aligned IB byte address: low 32 bits and high 16 bits. |
| 4 | Positive 20-bit IB DWORD count and VALID bit 23. |
| 5 | Remaining-DWORD count, 10. |
| 6–13 | Zero in the native caller. |
| 14–15 | Native completion signal handle. |

The count is neither bytes nor count-minus-one. The entire extent must be
accessible under the queue's mappings and fit the encoded address and count.
ROCr copies the IB, writes packet DWORDs 1–15, release-publishes DWORD 0, and
rings the doorbell. Its header barrier bit is clear and its acquire/release
scopes are caller parameters. [Native layout and publication][rocr-queue] ·
[PM4 fields][rocr-fields] · [Linux GC9.4.3 IB emission][linux-ib]

The synchronous call holds the shared-IB mutex through an acquired native
completion and destroys its temporary completion signal afterward. Providing a
signal selects the asynchronous path; return from that call is not a join. The
synchronous storage rule cannot be inferred for overlapping asynchronous uses
of the same IB. [Completion and shared-buffer ownership][rocr-queue]

## Virtual-XCC selection

ROCr wraps globally visible memory operations in PRED_EXEC when `NumXcc > 1`.
Its two words are `0xc0002300` and `0x01000000 | body_dword_count`: the 14-bit
count covers the following body, excluding the two prefix DWORDs, and bit 24
selects virtual XCC 0. This is a virtual queue context, not a physical XCC
number. [Predicate fields][rocr-fields] · [Actual multi-XCC caller][rocr-data]

The caller's reason is to execute an operation on globally visible addresses
once, rather than once per XCC. Other XCCs still participate in carrier
processing; predication does not terminate the IB's storage lifetime. Shader
workgroup routing has a different contract, described in [kernel
dispatch](dispatch.md#pm4-shader-state-inside-an-aql-queue). [ROCr routing
rationale][rocr-data]

## Confirmed data commands

The TC/L2 data selections in the GFX9-family MEC definitions admit the
following bounded encodings. PAL's constructors select LRU cache policy,
encode the addresses, and check alignment for the selected width. [COPY_DATA
constructor][pal-copy] · [WRITE_DATA constructor][pal-write]

| Operation | Header / control DWORDs | Remaining operands |
| --- | --- | --- |
| WRITE_DATA, one DWORD | `0xc0033700`, `0x00100200` | Destination low/high, immediate DWORD. |
| WRITE_DATA, two DWORDs | `0xc0043700`, `0x00100200` | Destination low/high, two immediate DWORDs. |
| COPY_DATA, 32 bits | `0xc0044000`, `0x00100202` | Source low/high, destination low/high. |
| COPY_DATA, 64 bits | `0xc0044000`, `0x00110202` | Source low/high, destination low/high. |

WRITE_DATA uses destination selection 2 in control bits 11:8, incrementing
addresses with bit 16 clear, and write confirmation at bit 20. COPY_DATA uses
source selection 2 in bits 3:0 and destination selection 2 in bits 11:8; bit
16 selects 64-bit rather than 32-bit data, and bit 20 requests write
confirmation. The zero cache-policy fields select LRU in these definitions.
[MEC COPY_DATA fields][pal-copy-fields] · [MEC WRITE_DATA
fields][pal-write-fields]

WRITE_DATA and COPY32 require four-byte alignment; COPY64 requires eight-byte
alignment at both memory operands. Two DWORDs in WRITE_DATA do not constitute
an atomic 64-bit store. COPY64's width likewise does not specify atomicity.
Confirmation makes the issuing command processor wait for the write's
confirmation; it is not a replacement for surrounding payload fences or the
host's acquired completion observation. [Address checks][pal-copy] ·
[WRITE_DATA confirmation][pal-write]

Address alignment is separate from source backing. The [COPY_DATA source-read
observation](../pm4/memory-commands.md#copy-source-backing) shows why a 32-bit
result is insufficient evidence for a four-byte native read footprint. The AQL
carrier adds publication and completion rules; its representation does not
specify a different source-fetch width for the embedded PM4 command.

## Mapping and visibility

System-memory placement does not alone select a cache policy. For native
GC9.4.3/9.4.4/9.5.0, Linux's PTE logic considers APU versus discrete device,
NUMA locality, VRAM ownership and partition, uncached requests, and extended
coherence. In its discrete-device branch, non-VRAM system memory on revisions
below GC9.5.0 receives UC with snooping. APU and local-memory branches differ.
This native predicate is more precise than a compiler target name. [GMC9 PTE
mapping][linux-coherence]

For coherent host/device storage, the complete data flow has separate edges:

1. The host initializes command bytes, source data, and native signal storage,
   then publishes writes for the actual mapping before the valid AQL header.
2. The carrier's acquire establishes the requested input visibility. The
   command processor executes the selected memory operations; a confirmed
   WRITE_DATA can feed the following COPY_DATA in the same IB.
3. Carrier release and native completion establish the selected outgoing
   visibility and completion boundary. The host acquires completion before
   observing destination data.
4. The host retains source, destination, IB, and signal storage through their
   last uses, and separately retires ring slots before reusing those slots.

SYSTEM acquire/release is the conservative scope for the host edges in this
composition. It does not repair an unsuitable mapping or supply shader
dependencies outside the command sequence. ROCr's memory-operation caller
explicitly uses SYSTEM release and waits on its native completion; its initial
acquire choice reflects that caller's own input protocol. [Native memory
caller][rocr-data] · [Carrier fences and join][rocr-queue] · [AQL fence
rules](barriers.md#fence-scope-and-observers)

## Rebuilding completed command storage

ROCr's synchronous shared-buffer path provides a concrete CPU serial-reuse
pattern: hold the buffer owner, copy commands, publish the carrier, acquire
native completion, and only then allow the next call to overwrite the shared
IB. This is distinct from reclaiming a ring slot or receiving an asynchronous
submission return. [Shared IB and completion][rocr-queue]

For a retained caller-owned IB, changing memory operands after that same
completed-use boundary preserves the ownership shape if every new command
extent and address remains valid for the native transport. CPU publication
must follow the complete rewrite and precede the next valid header. Payload
owners and independent consumers still need their own final-use joins. This
argument concerns serial command-storage reuse; it does not establish
GPU-generated commands, nested IBs, concurrent rewriting, or executable shader
replacement. [Native serial submission][rocr-queue] · [Executable
publication](dispatch.md#executable-publication-and-final-use)

Return to [AQL](README.md), [clock
capture](profiling.md#command-processor-clock-capture), or the [PM4
reference](../pm4/).

[rocr-queue]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_aql_queue.cpp#L1609-L1759
[rocr-data]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L4752-L4792
[rocr-fields]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/inc/amd_gpu_pm4.h#L65-L142
[linux-ib]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gfx_v9_4_3.c#L2950-L2982
[pal-copy]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9CmdUtil.cpp#L1055-L1160
[pal-write]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9CmdUtil.cpp#L4675-L4729
[pal-copy-fields]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_f32_mec_pm4_packets.h#L729-L918
[pal-write-fields]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_f32_mec_pm4_packets.h#L2571-L2669
[linux-coherence]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gmc_v9_0.c#L1100-L1159
