# PM4 cache control

The command processor can write back dirty cache data, invalidate cached copies,
and attach those actions to an execution dependency. A producer's completion,
the visibility of its writes, and a consumer's ability to read fresh data are
separate parts of that dependency. `ACQUIRE_MEM` supplies cache actions at the
command processor; `RELEASE_MEM` can perform cache actions after a pipeline
event and then publish a completion value.

This chapter follows ordinary compute callers in PAL and Mesa, with native
Linux packet definitions for GC12.1. GFX10/GFX11, PAL's GFX12, and GC12.1 have
different field meanings despite sharing the opcodes. The older GFX7–GFX9
control word is described separately. Compiler target gfx1250 identifies
GC12.1 in the cited Linux table; its name does not make the GFX12.0 encoding
applicable. [Native target mapping][linux-targets]

## Cache clients and dependencies

PAL's GFX10/GFX11 access model distinguishes instruction fetch, scalar K$,
vector V$, shared GL1, metadata M$, and GL2. Shader, command-processor,
indirect-argument, queue-atomic and timestamp accesses can use GL2. CPU and
external-memory accesses are classified as bypassing GL2. The required actions
depend on both endpoints, the resource's mapping and its view.
[Access classes][pal-clients] [Transition planner][pal-transitions]

| Transition in PAL's GFX10/GFX11 planner | Cache consequence |
| --- | --- |
| GL2 client writes → CPU or memory client reads | Write back GL2 before publishing completion to that consumer. |
| CPU or memory client writes → GL2 client reads | Invalidate GL2; PAL also writes it back so invalidation does not discard other dirty data. |
| Producer writes → shader reads | Invalidate the consumer's applicable scalar/vector/shared caches. Image metadata can add a metadata invalidate. |
| Shader read → compatible shader read | PAL can omit source-cache invalidation under its explicit non-global, no-write and compatible-view predicates. |

These are the planner's access classes, not a promise that every memory mapping
uses the same physical route. A system-memory address can still be cached by
the GPU. A GPU cache action neither chooses the host mapping policy nor performs
a host-language acquire operation. [Native mapping and handoff](handoff.md)
describes those additional premises.

On GFX10+, plain `ACQUIRE_MEM` does not wait for running shaders to become idle.
PAL's ordinary non-PWS barrier first joins the relevant producer stage, then
emits cache work. Mesa likewise emits the requested compute-completion event
before its acquire. The firmware predicates and owned-fence alternative for
`CS_PARTIAL_FLUSH` are in
[compute completion](memory-commands.md#compute-completion-and-firmware).
[Cache-only acquire][mesa-acquire] [PAL stage join][pal-join]
[Mesa stage join][mesa-join]

## Ordinary ACQUIRE_MEM representation

`ACQUIRE_MEM` is type-3 opcode `0x58`. The GFX10/GFX11 and GFX12 MEC definitions
use eight little-endian DWORDs, with header count 6:

| Word | MEC field | Units or meaning |
| --- | --- | --- |
| 0 | Type-3 header | Eight DWORDs including this header. |
| 1 | Reserved | Zero in the ordinary MEC form. Graphics forms have separate engine/stall controls. |
| 2 | `COHER_SIZE[31:0]` | Low part of the range size, in 256-byte units. |
| 3, bits 7:0 | `COHER_SIZE_HI` | High eight size bits; bits 31:8 reserved in the cited MEC definitions. |
| 4 | `COHER_BASE_LO` | Low part of the base, in 256-byte units. |
| 5, bits 23:0 | `COHER_BASE_HI` | High 24 base bits; bits 31:24 reserved. |
| 6, bits 15:0 | `POLL_INTERVAL` | Native polling control; PAL and Mesa emit 10. This is not a software timeout. |
| 7, bits 18:0 | `GCR_CNTL` | Generation-specific actions below; remaining bits reserved. |

[GFX10/GFX11 MEC layout][pal-mec] [GFX12 MEC layout][pal12-mec]
[Range conversion][pal-acquire] [PAL polling value][pal-poll]
[Mesa emitter][mesa-acquire]

For a byte interval `[base, base + length)`, PAL rounds the base down to 256
bytes and rounds `length + (base mod 256)` up to 256 bytes. Size is a unit count,
not count-minus-one. PAL's API uses zero base and zero size to request the whole
cache, then emits zero base and all defined size bits set. Range-selection bits
remain an independent choice; a large address interval is not a substitute for
whole-cache mode. [Alignment and full range][pal-acquire]

There is an unresolved engine-width discrepancy. Mesa emits size-high `0xff`
for MEC and `0xffffff` for GFX11-or-later graphics. PAL's shared GFX11 and GFX12
builders emit the wider ME size field even when called for compute, although
their generated MEC definitions reserve its upper 16 bits. Linux's GC12.1
packet header also masks size-high to eight bits, while its ring-sync builder
emits `0xffffff`. The sources establish the disagreement, not permission to
write reserved MEC bits or a reason to widen that field in a new encoder.
[Mesa engine predicate][mesa-acquire] [PAL GFX11 builder][pal-acquire]
[PAL GFX12 builder][pal12-acquire] [Linux header][linux121-acquire]
[Linux ring builder][linux121-sync]

### Acquire GCR fields

Bit positions below are relative to word 7. A dash denotes a field absent or
reserved in that cited layout, not an equivalent action elsewhere.

| Bits | GFX10/GFX11 PAL | GFX12 PAL | Native GC12.1 Linux |
| --- | --- | --- | --- |
| 1:0 | `GLI_INV` | `GLI_INV` | `GLI_INV` |
| 3:2 | `GL1_RANGE` | `GL1_RANGE` for K$/V$ | `GL1_RANGE` |
| 4 | `GLM_WB` | — | Part of `GL2_SCOPE[1:0]` at 5:4 |
| 5 | `GLM_INV` | — | Part of `GL2_SCOPE` |
| 6 | `GLK_WB` | `GLK_WB` | `GLV_WB` |
| 7 | `GLK_INV` | `GLK_INV` | `GLK_INV` |
| 8 | `GLV_INV` | `GLV_INV` | `GLV_INV` |
| 9 | `GL1_INV` | — | — |
| 10 | `GL2_US` | `GL2_US` | `GL2_US` |
| 12:11 | `GL2_RANGE` | `GL2_RANGE` | `GL2_RANGE` |
| 13 | `GL2_DISCARD` | `GL2_DISCARD` | `GL2_DISCARD` |
| 14 | `GL2_INV` | `GL2_INV` | `GL2_INV` |
| 15 | `GL2_WB` | `GL2_WB` | `GL2_WB` |
| 17:16 | `SEQ` | `SEQ` | `SEQ` |
| 18 | Unassigned in PAL's GCR union | `RANGE_IS_PA` | `GCR_RANGE_IS_PA` |

[GFX10/GFX11 GCR][pal-gcr] [GFX12 GCR][pal12-gcr]
[GC12.1 GCR][linux121-acquire]

`GLI_INV` values are 0 no action, 1 all, 2 range and 3 first/last. `GL1_RANGE`
uses 0 all, 2 range and 3 first/last, with 1 reserved. `GL2_RANGE` additionally
names 1 `VOL`. `SEQ` uses 0 parallel, 1 forward and 2 reverse. The ordinary PAL
builders leave `GL2_US`, `GL2_DISCARD` and physical-range selection zero; the
field names alone do not establish a useful selective-discard or physical-
address recipe. [Enumerations][mesa-gcr-enums] [PAL construction][pal-acquire]
[GFX12 construction][pal12-gcr]

GC12.1 adds explicit `GL2_SCOPE`: 0 device, 1 system, 2 force all writeback/
invalidation, 3 reserved. Linux's broad ring-sync sequence selects 2 and requests
GL2 writeback/invalidation plus instruction, scalar and vector invalidation.
That caller establishes a force-all sequence; it does not equate device and
system scope, or make scope bits optional when choosing a narrower operation.
[Scope values][linux121-acquire] [Ring sync][linux121-sync]

### Range and sequencing policy

PAL's GFX10/GFX11 builder uses GL1/GL2 range mode only for nonzero base and size
with size at most 64 KiB. This is a runtime heuristic, not a hardware size limit.
The builder explains that ranged operations translate virtual pages before
cache maintenance: walking an unmapped gap can fault, and walking a large range
can cost more than a whole-cache operation. Its GFX12 builder always selects
whole-cache mode, explicitly avoiding those page walks.
[Range heuristic][pal-range] [Threshold][pal-range-limit]
[GFX12 policy][pal12-gcr]

The sequence field orders cache actions relative to each other. It does not join
an unfinished shader. PAL requests forward order when scalar K$ writeback and
GL2 writeback occur together, so data released from a writable lower cache is
included by the outer writeback. Otherwise its ordinary GFX10/GFX11 acquire
uses parallel actions. PAL's GFX12 caller omits K$ writeback under its stated
read-only K$/no-SMEM-write compiler premise; that policy cannot be carried to
GC12.1's repurposed vector-writeback bit. [GFX10/GFX11 sequence][pal-acquire]
[GFX12 premise][pal12-gcr]

## RELEASE_MEM cache actions

`RELEASE_MEM`, opcode `0x49`, inserts an event whose cache actions precede its
completion publication. Emitting the packet does not make subsequent command
processing wait for that publication. A dependent stream still needs the
corresponding wait. The event, destination, confirmation and data fields are
described in [dispatch completion](dispatch.md#end-of-pipe-release-and-ownership).
[PAL release builder][pal-release] [Release/acquire caller][pal-split-release]

The following positions are in release word 1, not an acquire GCR word. A raw
copy of the acquire mask is incorrect:

| Word 1 bits | GFX10 | GFX11 | GFX12 PAL | GC12.1 Linux |
| --- | --- | --- | --- | --- |
| 12 | `GLM_WB` | `GLM_WB` | Reserved | Part of `GL2_SCOPE` at 13:12 |
| 13 | `GLM_INV` | `GLM_INV` | Reserved | Part of `GL2_SCOPE` |
| 14 | `GLV_INV` | `GLV_INV` | `GLV_INV` | `GLV_INV` |
| 15 | `GL1_INV` | `GL1_INV` | Reserved | Not defined |
| 16 | `GL2_US` | `GL2_US` | `GL2_US` | `GL2_US` |
| 18:17 | `GL2_RANGE` | `GL2_RANGE` | `GL2_RANGE` | `GL2_RANGE` |
| 19 | `GL2_DISCARD` | `GL2_DISCARD` | `GL2_DISCARD` | `GL2_DISCARD` |
| 20 | `GL2_INV` | `GL2_INV` | `GL2_INV` | `GL2_INV` |
| 21 | `GL2_WB` | `GL2_WB` | `GL2_WB` | `GL2_WB` |
| 23:22 | `SEQ` | `SEQ` | `SEQ` | `SEQ` |
| 24 | Reserved | `GLK_WB` | `GLK_WB` | `GLV_WB` |
| 26:25 | Cache policy | Cache policy | Temporal hint | Temporal hint |
| 30 | Reserved | `GLK_INV` | `GLK_INV` | `GLK_INV` |

[GFX10/GFX11 release fields][pal-release-fields]
[PAL release GCR][pal-gcr] [PAL GFX12 release][pal12-release]
[GFX12 packet layout][pal12-release-fields] [GC12.1 release][linux121-release]

Release has no instruction-cache invalidate. GFX10 also lacks the GFX11 release
scalar actions. PAL moves only representable actions into the release packet
and leaves the others for an acquire. In GFX11, scalar writeback with GL2
writeback selects forward order. Event-associated render-cache flushes already
precede the CP's GCR request; PAL does not request forward order merely because
such an event is present. [Action partition][pal-release-select]
[Release sequencing][pal-release] [GFX12 sequencing][pal12-gcr]

PAL's GFX10/GFX11 generic compute builder uses `BOTTOM_OF_PIPE_TS` with index 5.
Its source explains that ACE treats `CS_DONE` releases as the same end-of-pipe
operation, while graphics EOS events have different restrictions. The GFX12
sources also differ: Mesa allows PWS GCR actions with `CS_DONE`/`PS_DONE` at
GFX12 or later, whereas PAL's generic builder retains an EOP-only cache-action
assertion. PWS is a graphics-engine path in those callers, not a different MEC
wait encoding. [Compute event selection][pal-release-select]
[Mesa PWS release][mesa-pws-release] [PAL GFX12 release][pal12-release]

Linux's GC12.1 ring fence explicitly combines vector writeback, GL2 writeback,
force-all scope and forward sequencing. This is a concrete caller for the
repurposed bit 24. Clearing it because an older GFX12 path omits scalar
writeback would discard a different cache action. The Linux fence also owns
its event, temporal hint, interrupt and destination policy; those fields do
not come from the acquire mask. [GC12.1 fence][linux121-fence]

## Metadata and source disagreements

PAL says GLM writeback is unimplemented and leaves it clear in both acquire
and release. Its image planner still requests GLM invalidation: compressed
image writes can read metadata during GL2 read-modify-write. A compute image
operation therefore does not become metadata-free because no graphics stage
uses it. View changes and direct versus indirect metadata access add their
own cache dependencies. [PAL omission][pal-acquire]
[Image access planner][pal-transitions]

RADV instead sets both GLM writeback and invalidation for pre-GFX12 L2
writeback/invalidation or metadata invalidation, including ordinary buffer
barriers. Linux's broad GFX11 ring flush also sets both. Those choices preserve
a real source disagreement: PAL's statement does not prove every GLM_WB use
redundant, and RADV's combination does not establish that every GLM_INV requires
GLM_WB. A hardware-effect conclusion requires an architecture/firmware contract
that resolves the mismatch. [RADV emitter][mesa-cache]
[Buffer-barrier caller][mesa-buffer] [Storage-write lowering][mesa-buffer-access]
[Linux ring flush][linux11-sync]

SDMA's GCR and scoped-transfer paths have different fields and callers. Their
omission of metadata controls does not settle PM4's M$ behavior.
[SDMA cache operations](../sdma/cache.md)

## Graphics PWS and older control words

Graphics PWS acquire uses 128-byte GCR base/size units and a 25-bit high-size
field, unlike the ordinary 256-byte MEC form. PAL moves a requested PWS wait
to PFP or ME when cache work is attached; Mesa asserts that GCR has no effect
at the other PWS stages. These fields and counters belong to the graphics
pipeline. [PAL PWS acquire][pal-pws] [Mesa PWS acquire][mesa-pws-acquire]
[GFX11 ME size fields][pal-me-sizes]

GFX7–GFX9 MEC uses a seven-DWORD `ACQUIRE_MEM`, count 5, with `CP_COHER_CNTL`
in word 1 and no trailing GCR word. Mesa also uses that form for GFX9 graphics;
older graphics uses `SURFACE_SYNC`. The GFX9 control includes:

| Word 1 bit | Native control |
| --- | --- |
| 3 / 4 | `TC_NC_ACTION_ENA` / `TC_WC_ACTION_ENA` |
| 5 | `TC_INV_METADATA_ACTION_ENA` |
| 15 | `TCL1_VOL_ACTION_ENA` |
| 18 | `TC_WB_ACTION_ENA` |
| 22 / 23 | `TCL1_ACTION_ENA` / `TC_ACTION_ENA` |
| 27 / 28 | `SH_KCACHE_ACTION_ENA` / `SH_KCACHE_VOL_ACTION_ENA` |
| 29 / 30 | `SH_ICACHE_ACTION_ENA` / `SH_KCACHE_WB_ACTION_ENA` |

[Legacy packet emission][mesa-acquire] [GFX9 fields][mesa9-fields]

RADV's legacy path couples full L2 invalidation to L1 invalidation and, on
GFX8+, L2 writeback. For writeback without L2 invalidation it adds
`TC_NC_ACTION_ENA`, with the explicit premise that its memory types are
non-coherent (`MTYPE <= 1`), and emits vector-cache invalidation separately.
These are older cache-operation combinations, not aliases of a GFX10 GCR mask.
The source describes the older acquire as waiting for caches to assert idle;
that wording is not a universal shader-stage completion contract.
[Legacy caller and mapping premise][mesa-legacy] [Acquire behavior][mesa-acquire]

## Complete producer-to-consumer sequence

A compute producer and shader consumer on GFX10/GFX11 can use the following
sequence. PAL's split barriers supply the concrete release/wait/acquire path;
its command-buffer allocator owns the fence storage.

1. Establish the payload mappings, shader-access modes and a control cell whose
   updates are visible to the waiting CP. Prepare commands and arguments before
   publishing their queue references. Initialize the cell before its first
   protocol use, either through visible host stores or an ordered GPU reset;
   PAL emits its initial zero-write into the command stream.
2. Dispatch the producer. Emit an EOP release with the producer-side actions
   required for its actual consumer, then a known-value completion write. If
   CP DMA also produces the payload, join it through its separate
   [DMA completion contract](dma.md#combined-release-wait-and-firmware-identity).
3. Wait for the corresponding control value before the consumer. PAL's token
   path uses allocated, initialized 32-bit fence cells and a memory wait; an
   event path has its own value/reset protocol. A wait cannot acquire the
   payload caches by itself.
4. Emit the required consumer invalidations after the wait, then dispatch the
   consumer. Instruction fetch requires GLI invalidation when the executable's
   publication contract calls for it; a data-only release cannot supply it.
5. Retain the control cell through every observer, the payload through the last
   consumer, and code/arguments through their final dispatch. Reuse command
   storage only after its separate submission-retirement contract is met.

[Release and CP-DMA ordering][pal-split-release]
[Wait before acquire][pal-split-acquire] [Fence allocation][pal-fence-owner]
[Event contract][pal-event] [Storage retirement](command-buffers.md)

For a CPU consumer, the final edge instead combines the GPU's payload release,
a host-visible completion mapping, and the host's acquire observation before
reading. For another GPU or a transfer engine, its access path supplies the
consumer-side actions. The operation is selected by that edge, not merely by
the address being in host or device memory. [Programming recipes](../recipes/README.md)

[linux-targets]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_device.c#L416-L475
[pal-clients]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9AcquireReleaseBarrier.cpp#L56-L68
[pal-transitions]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9AcquireReleaseBarrier.cpp#L278-L395
[mesa-acquire]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/common/ac_cmdbuf_cp.c#L398-L449
[pal-join]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9AcquireReleaseBarrier.cpp#L1998-L2042
[mesa-join]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_cs.c#L210-L244
[pal-mec]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_f32_mec_pm4_packets.h#L57-L136
[pal12-mec]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/chip/gfx12_merged_f32_mec_pm4_packets.h#L57-L136
[pal-acquire]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9CmdUtil.cpp#L600-L715
[pal12-acquire]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12CmdUtil.cpp#L1714-L1752
[linux121-acquire]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gfx_v12_1_pkt.h#L440-L523
[linux121-sync]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gfx_v12_1.c#L4159-L4178
[pal-gcr]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9CmdUtil.cpp#L228-L273
[pal12-gcr]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12CmdUtil.cpp#L1590-L1666
[mesa-gcr-enums]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/registers/pkt3.json#L63-L91
[pal-range]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9CmdUtil.cpp#L589-L608
[pal-range-limit]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9CmdUtil.h#L300-L309
[pal-release]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9CmdUtil.cpp#L3422-L3539
[pal-split-release]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9AcquireReleaseBarrier.cpp#L1321-L1510
[pal-release-fields]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_f32_mec_pm4_packets.h#L1953-L2074
[pal12-release]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12CmdUtil.cpp#L1831-L1919
[pal12-release-fields]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/chip/gfx12_merged_f32_mec_pm4_packets.h#L1987-L2142
[linux121-release]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gfx_v12_1_pkt.h#L314-L399
[pal-release-select]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9CmdUtil.cpp#L3306-L3354
[mesa-pws-release]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/common/ac_cmdbuf_cp.c#L179-L235
[linux121-fence]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gfx_v12_1.c#L3737-L3768
[mesa-cache]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_cs.c#L74-L111
[mesa-buffer]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_cmd_buffer.c#L16258-L16301
[linux11-sync]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gfx_v11_0.c#L6846-L6866
[pal-pws]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9CmdUtil.cpp#L721-L792
[mesa-pws-acquire]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/common/ac_cmdbuf_cp.c#L146-L177
[mesa9-fields]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/registers/gfx9.json#L10766-L10782
[mesa-legacy]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_cs.c#L438-L501
[pal-split-acquire]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9AcquireReleaseBarrier.cpp#L1711-L1793
[pal-fence-owner]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfxCmdBuffer.cpp#L480-L501
[pal-event]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/inc/core/palCmdBuffer.h#L2818-L2862
[pal-poll]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/device.h#L1023
[pal-me-sizes]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_f32_me_pm4_packets.h#L140-L164
[mesa-buffer-access]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_cmd_buffer.c#L7917-L7954
