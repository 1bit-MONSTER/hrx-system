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

For GFX10/GFX11 MEC, ACQUIRE_MEM is eight DWORDs. Word 1 is reserved; size
low/high occupy words 2–3, base low/high words 4–5, polling interval word 6,
and 19-bit GCR word 7. Base and size use 256-byte units. The MEC high-size
field has eight bits; its high-base field has 24. Full range uses zero base
and all defined size bits set, with reserved bits zero. Mesa explicitly keeps
MEC size-high at `0xff`, unlike the wider GFX11 graphics form.
[MEC range layout][acquire-layout] [Address units][acquire-units]
[Mesa engine distinction][mesa-acquire]

PAL's generic acquire builder uses the wider graphics-style GFX11 size field.
That shared implementation does not authorize the extra reserved MEC bits.
The GCR action fields themselves are also generation specific:

| GFX10/GFX11 acquire field | Meaning |
| --- | --- |
| GLI_INV bits 1:0 | 1 means all instruction cache; 3 is the separate FIRST_LAST mode. |
| GLM_WB / GLM_INV bits 4 / 5 | Metadata writeback/invalidation; the source disagreement below is material. |
| GLK_WB / GLK_INV bits 6 / 7 | Scalar cache actions. |
| GLV_INV / GL1_INV bits 8 / 9 | Vector and shared first-level cache invalidation. |
| GL2_INV / GL2_WB bits 14 / 15 | Last-level data-cache invalidation/writeback. |
| SEQ bits 17:16 | Parallel 0, forward 1, reverse 2. |

[GCR layout][gcr-fields] [GLI/sequence values][mesa-fields]

PAL omits GLM_WB as unimplemented in hardware and uses sequential order when
scalar GLK writeback accompanies GL2 writeback. Its image planner can still
require GLM_INV for shader writes because metadata read-modify-write reads the
metadata cache. Compute images are not automatically metadata-free.
[PAL acquire][pal-acquire] [PAL release][pal-release]
[Image metadata policy][pal-metadata]

RADV couples pre-GFX12 L2 writeback/invalidation or metadata invalidation to
both GLM_WB and GLM_INV, including ordinary buffer barriers; Linux's broad
GFX11 ring flush also includes both. These source choices do not establish
that GLM_INV requires GLM_WB, or that GLM_WB is always a no-op. They remain a
hardware-effect disagreement. [Buffer caller][mesa-buffer]
[Access mapper][mesa-source] [RADV cache emitter][mesa-barrier]
[Linux ring recipe][linux-barrier]

ROCr's SDMA GCR omission of both GLM fields is a separate engine contract. Its
non-DXG gfx11.5 factory instead selects scoped packets without that GCR form.
It cannot resolve PM4 metadata-cache behavior.
[SDMA builder][rocr-sdma-gcr] [Factory][rocr-sdma-selection]
[Template capabilities][rocr-sdma-scopes]

GFX12.0 reserves the older GLM bits 4–5 and GL1 bit 9. Native GC12.1 repurposes
bits 4–5 as GL2_SCOPE and bit 6 as GLV_WB; Linux maps that native IP to compiler
target gfx1250. A shared opcode or a compiler target prefix cannot substitute
for these native field definitions. RELEASE_MEM also has its own GCR layout,
covered in [dispatch](dispatch.md#end-of-pipe-release-and-ownership).
[GFX12 layout][pal-gfx12-gcr] [GC12.1 fields][linux-gfx121-gcr]
[Target mapping][linux-targets]

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
[acquire-layout]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_f32_mec_pm4_packets.h#L57-L136
[acquire-units]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9CmdUtil.cpp#L662-L684
[gcr-fields]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9CmdUtil.cpp#L228-L250
[mesa-fields]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/registers/pkt3.json#L78-L91
[pal-acquire]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9CmdUtil.cpp#L687-L709
[pal-release]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9CmdUtil.cpp#L3485-L3532
[pal-metadata]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9AcquireReleaseBarrier.cpp#L278-L367
[mesa-buffer]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_cmd_buffer.c#L16258-L16301
[mesa-source]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_cmd_buffer.c#L7918-L7954
[mesa-barrier]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_cs.c#L74-L111
[linux-barrier]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gfx_v11_0.c#L6846-L6866
[rocr-sdma-gcr]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L2992-L3026
[rocr-sdma-selection]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L853-L884
[rocr-sdma-scopes]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/inc/amd_blit_sdma.h#L579-L590
[pal-gfx12-gcr]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12CmdUtil.cpp#L1598-L1620
[linux-gfx121-gcr]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gfx_v12_1_pkt.h#L453-L491
[linux-targets]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_device.c#L416-L475
