# PM4 memory commands

Command-processor memory operations move data, publish control values and wait
for dependencies. Their packet width is not an atomicity guarantee, write
confirmation is not a shader join, and a satisfied memory wait does not itself
acquire a payload cache. A complete sequence binds each operation to an engine,
native mapping, producer/consumer pair and storage owner.

The forms below follow PAL's GFX10/GFX11 MEC definitions and ordinary PAL/Mesa
callers. The `gfx9` source-directory name also contains later-generation
formats. Generation-specific differences are stated where they change the
packet or the execution rule.

## Representation

Packets contain little-endian 32-bit words. A type-3 header has type 3 in bits
31:30, `(total_dwords - 2)` in bits 29:16 and opcode in bits 15:8. PAL's MEC
header names the low byte reserved; shared ME/PFP builders and some compute
callers use specific low bits, so the applicable caller matters.
[Header layout][header] [Header builder][header-builder]

| Operation | Ordinary memory form |
| --- | --- |
| COPY_DATA, `0x40` | Six DWORDs. Source selector bits 3:0; destination bits 11:8; count bit 16 selects 32/64 bits; confirmation bit 20. Source low/high are words 2–3, destination low/high 4–5. TC/L2 source and destination are selector 2. Alignment is 4/8 bytes for the selected width. |
| WRITE_DATA, `0x37` | Four fixed DWORDs followed by payload. Destination selector bits 11:8; bit 16 clear means incrementing addresses; confirmation bit 20. Four-byte-aligned destination low/high are words 2–3. TC/L2 destination is 2; MEMORY is 5. |
| WAIT_REG_MEM, `0x3c` | Seven DWORDs. Function bits 2:0; memory space 1 in bits 5:4; ordinary wait operation 0. Address low/high at words 2–3, reference at 4, mask at 5, polling controls at 6. Memory address is four-byte aligned. |
| WAIT_REG_MEM64, `0x93` | Nine DWORDs. Eight-byte-aligned address, reference low/high at words 4–5, mask low/high at 6–7, polling controls at 8. |
| NOP, `0x10` | Ordinary type-3 count describes a packet of at least two DWORDs. PAL also emits a special one-DWORD NOP with count `0x3fff`. Padding requirements come from the containing transport. |

[Copy layout][copy-layout] [Copy builder][copy-builder]
[Write layout][write-layout] [Write builder][write-builder]
[Wait layouts][wait-layout] [Wait builders][wait-builders]
[Opcodes][opcodes] [NOP builder][nop-builder]

The type-3 count can represent a WRITE_DATA payload of at most 16,381 DWORDs
with its four-word fixed body. This representation limit is not a ring-size or
all-firmware execution guarantee. PAL normally selects LRU policy and write
confirmation; Mesa's ordinary WRITE_DATA emitter also requests confirmation.
[Header builder][header-builder] [Write construction][write-builder]
[Mesa write][mesa-write]

## Copy source backing

COPY_DATA's selected result width and address alignment do not establish the
source read footprint. PAL checks four-byte alignment for a 32-bit copy and
eight-byte alignment for a 64-bit copy; its builder does not describe trailing
source backing. [Copy address checks][copy-builder]

An experiment on gfx1100 (PCI `1002:744c`, revision `0xc8`, Linux
`6.17.0-41-generic`, KFD topology `fw_version` 2650) observed a CPC read into the
following page when a confirmed TC/L2-to-TC/L2 32-bit copy selected the final
DWORD of a system-memory source mapping. Earlier copies in the same sequence
completed, but this copy and the subsequent completion write did not.

Keeping the following 4 KiB source page explicitly owned, initialized and
GPU-readable allowed the same copy positions and packet fields to complete.
The destination remained a single 4 KiB mapping: only the selected DWORDs
changed, including its last DWORD, and all source words remained unchanged.
The corresponding 64-bit copies completed with a single source page and the
last selected QWORD ending at its boundary.

This observation establishes a source-backing requirement for that deployment,
not an exact fetch width, minimum padding size or firmware fix version. In
particular, a 64-bit source fetch for a 32-bit result is a possible explanation,
not a measured bound. A layout using the 32-bit form keeps its readable trailing
backing alive through completion instead of deriving source reach from count
bit 16. Other generations, firmware revisions and queue transports need their
own evidence for that boundary.

## Waits, signaling and visibility

The function enumeration is always-pass 0, LT 1, LE 2, EQ 3, NE 4, GE 5 and
GT 6. The mask selects the memory operand's compared bits. These definitions
alone do not specify full-width signedness for every relational function or
what a reference containing bits outside its mask means. An unsigned C type
is not hardware signedness evidence. [Wait representation][wait-layout]

Ordinary waits, preemptable waits and write/wait/write operations are distinct
operation fields. ACE offload is another control: PAL enables bit 31 of the
polling word for compute waits, while Mesa's ordinary memory wait emits polling
interval 4 without that bit. PAL uses its interval constant 10. These are
actual caller choices; one form's behavior does not establish the other.
[PAL waits][wait-builders] [Mesa ordinary wait][mesa-wait]

A producer-to-consumer memory protocol has four ordered parts:

```text
complete producer accesses → release payload to the required visibility domain
  → publish a control value → consumer wait succeeds
  → acquire consumer caches → consume payload
```

The control cell itself must be fresh to the waiting engine before the later
payload acquire. Its producer must not expose a satisfying value before the
payload release. A multiword copy or WRITE_DATA payload is not thereby an
indivisible update; protocols using a wide control value need a corresponding
atomicity contract or a value/update sequence safe under the actual observation
rules. [Event release/acquire contract][event-contract]

Storage follows the last consumer. A producer's completion can release its
private input while the produced payload and signal remain borrowed by another
queue. A completion marker can also precede notification or command tails.
[Cross-queue handoff](handoff.md) and [command buffers](command-buffers.md)
describe those separate ownership boundaries.

## Atomic operations and participants

`ATOMIC_MEM` performs a selected 32- or 64-bit TC/GL2 operation. Its arithmetic,
command mode, returned-value transport, participant domain and surrounding
cache dependency have separate contracts. The [atomic operations](atomics.md)
chapter supplies the complete PAL integer conversion, packet fields, native
SWAP/CMPSWAP callers, host-participation premises and storage ownership.

## Compute completion and firmware

ACQUIRE_MEM cache work on GFX10+ does not wait for shader idle. A preceding
shader producer therefore needs an execution join before its release/cache
operations. PAL's `CanUseCsPartialFlush` applies these predicates:

| Engine/generation | Predicate |
| --- | --- |
| Graphics-capable engine | Accepted by this predicate. |
| Non-graphics GFX10.1 | ME firmware feature version at least 32 and `disableAceCsPartialFlush` clear. |
| Non-graphics GFX10.3 | ME firmware feature version at least 35 and that setting clear. |
| GFX11/11.5 | Outside this GFX10-specific restriction. |

[Event predicate and constants][pal-cs-gate] [Family normalization][pal-gfx10]
[Cache-only acquire][mesa-acquire]

PAL attributes the restriction to CWSR but does not inspect whether CWSR is
active on the queue. Its `cpUcodeVersion` comes from the ME firmware query's
`feature` output, not the image `ver`, a MEC image version, or a topology field
with a similar name. The comment is not a complete hardware erratum matrix.
[Firmware assignment][pal-cs-firmware] [Linux query fields][linux-cs-firmware]

When the event is unavailable, PAL's compute-idle builder uses owned 32-bit
fence storage: initialize if necessary, issue a known-value BOTTOM_OF_PIPE_TS
RELEASE_MEM, then equality WAIT_REG_MEM. The storage survives the wait, and
required cache operations remain separate. [Idle alternative][pal-cs-wait]
[Release event][pal-cs-release] [Queue-owned fence][pal-cs-storage]
[Following cache work][pal-cs-cache]

RADV directly emits CS_PARTIAL_FLUSH on its traced compute barrier path without
the same firmware gate. Its DRM queue construction differs from KFD CWSR
construction, but the inspected DRM setup preserves register state and does not
prove CWSR is unreachable. The discrepancy is unresolved; neither source
establishes that the other's firmware predicate can be dropped.
[RADV emission][mesa-cs-event] [RADV device boundary][mesa-cs-admission]
[KFD context][linux-cs-kfd] [DRM context][linux-cs-drm]

## Cache and architecture boundary

`ACQUIRE_MEM` and `RELEASE_MEM` have different cache-control layouts. Their
range units, instruction/scalar/vector/metadata actions, scope and sequencing
also vary by native generation and engine. The [cache-control chapter](cache.md)
compares those fields, explains the older control word and graphics PWS form,
and traces a complete producer/release/wait/acquire flow. It retains the
MEC range-width and metadata-writeback source disagreements explicitly.

[header]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_f32_mec_pm4_packets.h#L43-L54
[header-builder]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9CmdUtil.cpp#L285-L301
[copy-layout]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_f32_mec_pm4_packets.h#L730-L916
[copy-builder]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9CmdUtil.cpp#L980-L1167
[write-layout]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_f32_mec_pm4_packets.h#L2571-L2670
[write-builder]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9CmdUtil.cpp#L4639-L4732
[wait-layout]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_f32_mec_pm4_packets.h#L2285-L2567
[wait-builders]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9CmdUtil.cpp#L4439-L4594
[opcodes]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_pm4_it_opcodes.h#L65-L130
[nop-builder]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9CmdUtil.cpp#L2744-L2771
[mesa-write]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/common/ac_cmdbuf_cp.c#L57-L86
[mesa-wait]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/common/ac_cmdbuf_cp.c#L89-L103
[event-contract]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/inc/core/palCmdBuffer.h#L2818-L2862
[pal-cs-gate]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9CmdUtil.cpp#L367-L445
[pal-gfx10]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/device.h#L2430-L2433
[mesa-acquire]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/common/ac_cmdbuf_cp.c#L398-L449
[pal-cs-firmware]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/os/amdgpu/amdgpuDevice.cpp#L975-L985
[linux-cs-firmware]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_kms.c#L237-L240
[pal-cs-wait]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9CmdUtil.cpp#L4287-L4336
[pal-cs-release]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9CmdUtil.cpp#L3342-L3353
[pal-cs-storage]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9QueueContexts.cpp#L235-L244
[pal-cs-cache]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9AcquireReleaseBarrier.cpp#L2027-L2042
[mesa-cs-event]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_cs.c#L221-L228
[mesa-cs-admission]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_physical_device.c#L146-L157
[linux-cs-kfd]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_mqd_manager_v10.c#L130-L142
[linux-cs-drm]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gfx_v10_0.c#L6980-L7017
