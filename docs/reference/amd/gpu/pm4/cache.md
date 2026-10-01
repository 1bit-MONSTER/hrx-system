# PM4 cache control

The command processor can write back dirty cache data, invalidate cached copies,
and attach those actions to an execution dependency. A producer's completion,
the visibility of its writes, and a consumer's ability to read fresh data are
separate parts of that dependency. `ACQUIRE_MEM` supplies cache actions at the
command processor; `RELEASE_MEM` can perform cache actions after a pipeline
event and then publish a completion value.

This chapter follows ordinary compute callers in PAL, Mesa and ROCr, with
native Linux emitters for GC9.4.3/4 and GC12.1. GFX10/GFX11, PAL's GFX12,
and GC12.1 have different field meanings despite sharing the opcodes. The
older GFX7–GFX9 control word is described separately. Compiler target gfx1250
identifies GC12.1 in the cited Linux table; its name does not make the GFX12.0 encoding
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

### GFX12 CP and shader handoffs

PAL's GFX12 planner describes shader GL2 as coherent across shader engines,
while CP accesses MALL directly. Its bypass-GL2 class includes CP, indirect
arguments, queue atomics and timestamps as well as CPU and memory accesses.
Those CP clients belong to the GL2 class in its GFX10/GFX11 planner. The same
producer/consumer operation can therefore need different outer-cache work.
[GFX12 client classes][pal12-clients] [GFX10/GFX11 classes][pal-clients]
[GFX12 topology and history][pal12-transition-history]

The GFX12 planner first resolves copy/clear/resolve access masks using the
actual command-buffer engine history and resource kind. For example, buffer
copies can leave shader GL2 stale through CP, while image copies use the
shader path. It then applies these GL2 rules in order:

| Source access | Destination access | GL2 action |
| --- | --- | --- |
| Includes a bypass-GL2 client | Includes a GL2 client, or is unknown in a split release | Invalidate and write back, preserving other valid dirty data. |
| Includes a GL2 client, or is unknown in a split acquire | Includes a bypass-GL2 client | Write back to make the bypass route observe the data. |

[Access normalization][pal12-normalize] [Ordered GL2 rules][pal12-transition-history]

The source need not be a writer in the immediately preceding operation. For
`shader write → shader read → CP read`, the last transition still needs GL2
writeback: the shader read does not establish that the earlier dirty data
reached MALL. PAL receives one transition, rather than the full resource access
history, and conservatively retains this writeback. A read-to-read label alone
cannot establish that a cache operation is redundant.
[History limitation][pal12-transition-history]

Split buffer/global barriers make the missing information explicit:
`ReleaseInternal` passes destination access zero, while `AcquireInternal`
passes source access zero. Zero does not make every split half a full flush.
For example, a shader-only release with an unknown destination contributes no
GL2 action from the two routing rules; a later acquire for a CP reader requests
GL2 writeback. On the non-PWS path, PAL joins the release token with
`WAIT_REG_MEM` before emitting the remaining `ACQUIRE_MEM` actions.
[Split release][pal12-split-source] [Split acquire][pal12-split-destination]
[Token wait][pal12-token-wait] [Following cache work][pal12-acquire-tail]

Shader front-end caches retain their own requirement. A destination shader
read requests K$/V$ invalidation unless PAL's non-global, read-only source
and compatible-view conditions permit omission. The absence of a GL2 action
does not remove that work or the execution dependency. The command buffer's
CP-DMA token path can defer both the DMA join and its cache work until acquire;
the cache operations remain after DMA completion so an unfinished copy cannot
make GL2 stale again after it was refreshed.
[Shader cache conditions][pal12-front-end] [Deferred DMA release][pal12-deferred-dma]
[DMA acquire and cache planning][pal12-acquire-dma]

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

### Metadata addressing and subresource ranges

PAL's GFX10/GFX11 image path has an additional GL2 writeback/invalidation rule
for pipe-misaligned metadata. Its explanation identifies different metadata
addressing by the render backends and texture cache. Direct metadata access
includes color/depth use and shaders that read or update metadata explicitly;
indirect access includes shader image reads and writes through resource views.
A compute layout-transition shader can use the direct mode, so
`CoherShaderWrite` alone does not identify an indirect access.
[Addressing premise][pal-metadata-layout] [Access modes][pal-metadata-transition]
[Layout-transition caller][pal-metadata-blit]

Image finalization records the first affected mip for each plane. `UINT_MAX`
means none and zero means every mip. A subresource range needs the workaround
when its highest included mip reaches that plane's threshold for any covered
plane. The decision concerns the image layout and subresource range, rather
than the allocation's base-address alignment alone.
[Finalization caller][pal-metadata-finalize]
[Threshold construction][pal-metadata-layout] [Range query][pal-metadata-range]

The pinned producer derives these intermediate values:

| Value | PAL calculation |
| --- | --- |
| `B`, `S` | `log2(bitsPerTexel / 8)` and `log2(sampleCount)`. |
| `P`, `F` | Native `GB_ADDR_CONFIG.NUM_PIPES` and `MAX_COMPRESSED_FRAGS` field values. |
| `C`, GFX10.1 | `min(6, B' + S)`, where `B' = 2` for depth/stencil images with at least eight array slices, otherwise `B`. |
| `C`, GFX10.3/GFX11 | `B + S`. |
| `O`, `SO`, `D` | `max(C + P - 8, 0)`, `min(S, O)` and `max(S - F, 0)`, respectively. |

Here PAL's `IsGfx11` means its `GfxIp11_0` or `GfxIp11_5` enum, and its
`IsGfx103Plus` test is an enum comparison above `GfxIp10_1`. The calculation
belongs to this GFX10/GFX11 image path. The first-affected-mip rules are:

| Image condition | First affected mip |
| --- | --- |
| GFX11 image has a DCC or HTILE metadata mip tail and more than one mip | First mip reported in that tail by the address library; later rules can lower it to zero. |
| Depth/stencil has HTILE and permits metadata texture fetch, with non-power-of-two VRAM bus width or `O > 0` | Zero. |
| GFX11 color has DCC and permits metadata texture fetch, with non-power-of-two VRAM bus width or `O > 0` | Zero. |
| Earlier color path has non-power-of-two VRAM bus width or `SO > D`, and either texture-fetchable DCC or shader-readable compressed FMASK without DCC | Zero. |
| No applicable condition | `UINT_MAX`. |

[Layout predicates][pal-metadata-predicates] [Mip-tail query][pal-metadata-tail]
[GFX11 identity][pal-gfx11-identity] [GFX10.3 predicate][pal-gfx103-identity]

The barrier planner conservatively treats a global transition as potentially
covering such metadata; an image transition can use the range query, while an
ordinary buffer transition has no image metadata. Applicable writes request
GL2 writeback and invalidation across access modes. A split release lacks the
destination access mask, so it retains this refresh for an eligible metadata
writer instead of assuming that the next access uses the same mode.
[Planner and exemptions][pal-metadata-transition]
[Resource-specific inputs][pal-metadata-inputs]

PAL's ordinary access-mask path considers `CoherColorTarget`,
`CoherDepthStencilTarget`, `CoherShaderWrite` and `CoherPresent` sources. It
removes buffer-only categories from both masks and the separately handled BLT
destination categories from the source. It omits this metadata-refresh
contribution for either of these cases:

| Source and destination | Additional premise |
| --- | --- |
| Both masks are exactly `CoherColorTarget`, or both exactly `CoherDepthStencilTarget` | Both accesses use the same direct mode. |
| Both masks include `CoherShaderWrite` and contain only `CoherShader` bits | The caller establishes `shaderMdAccessIndirectOnly`; a layout-transition BLT does not establish this premise. |

[Exact exemptions][pal-metadata-transition]
[Layout-transition input][pal-metadata-blit]

Generic copy/clear/resolve destinations take a separate path. It consults the
command buffer's retained direct/indirect metadata-write history and the
original source mask before the generic flags lose their meaning. A direct-only
history can omit refresh when the destination mask is exactly a color target
or a depth/stencil target. An indirect-only history can omit it when the
destination includes shader writes, contains only shader bits, and satisfies
the same indirect-only premise. Both exemptions require that the source contain
only BLT destination categories after removing buffer-only categories. Mixed
direct/indirect history retains the refresh. Ordinary BLT cache-dirty flags
alone are insufficient because clearing them need not have refreshed GL2.
This GL2 operation is distinct from both
ordinary GLM invalidation and the disputed `GLM_WB` bit.
[BLT history and exemptions][pal-metadata-blit-history]
[Original-mask requirement][pal-metadata-transition]

## Graphics PWS

Graphics PWS acquire uses 128-byte GCR base/size units and a 25-bit high-size
field, unlike the ordinary 256-byte MEC form. PAL moves a requested PWS wait
to PFP or ME when cache work is attached; Mesa asserts that GCR has no effect
at the other PWS stages. These fields and counters belong to the graphics
pipeline. [PAL PWS acquire][pal-pws] [Mesa PWS acquire][mesa-pws-acquire]
[GFX11 ME size fields][pal-me-sizes]

### Consumer stage and deferred waits

RADV `44cc4ca677a4` derives the latest permissible PWS acquire point from the
barrier's destination stages, independently of the producer's completion and
cache actions. It combines pending destinations by retaining the earliest
required point. The source policy is:

| Destination after stage expansion | Required point |
| --- | --- |
| Indirect draw/copy, index input, conditional rendering or command preprocessing | PFP; the stage-flush producer also requests `PFP_SYNC_ME`. |
| Only early/late fragment tests, fragment shading or color attachment output | `PRE_DEPTH` may be used. |
| Other nonempty stages, including compute | ME. |
| No destination stage | No new stage requirement; other pending work still supplies its own requirement. |

[Destination classification and merge][mesa-pws-destination]
[PFP synchronization producer][mesa-pfp-destination]

For its GFX11+ render-cache release path, RADV attaches the data-cache work to
`RELEASE_MEM`. The matching acquire retains only a requested instruction-cache
invalidate. That invalidate forces an ME/PFP wait; a `PRE_DEPTH` acquire has no
GCR actions. Ordinary draws and mesh draws permit the later point, while
compute dispatch, ray dispatch and command-buffer finalization do not.
Device-generated draws also exclude deferral because the command processor
consumes their generated commands before fragment processing.
[Release/acquire partition][mesa-pws-partition]
[Draw caller][mesa-pws-draw] [Mesh caller][mesa-pws-mesh]
[Compute caller][mesa-pws-compute] [Ray caller][mesa-pws-ray]
[Finalization caller][mesa-pws-finalize] [Deferral clamp][mesa-pws-resolve]

The actual acquire starts at PFP when `PFP_SYNC_ME` is pending, otherwise ME;
the destination policy can then choose ME or `PRE_DEPTH` subject to those
constraints. Only an actual PFP acquire consumes the pending PFP synchronization.
An ME or `PRE_DEPTH` acquire leaves that obligation for the separate
`PFP_SYNC_ME` emission. Thus a completed release and a later pipeline wait do
not, by themselves, order an earlier parser read. This composition is a
graphics-ring protocol: the shared PWS builder requires `AMD_IP_GFX` and
GFX11 or later. An ordinary compute-ring acquire keeps its separate encoding
and producer-completion contract.
[Stage selection][mesa-pws-partition] [Remaining PFP wait][mesa-pws-pfp-tail]
[PWS builder predicates][mesa-pws-builder]

## GFX7–GFX9 and CDNA control words

Legacy compute cache maintenance places actions in `CP_COHER_CNTL`, without a
GCR word. Linux selects `gfx_v9_4_3` for physical GC9.4.3 and GC9.4.4; KFD maps
both to compiler target gfx942. This native selection, rather than a source
directory named `gfx9`, identifies the CDNA caller below.
[Native driver selection][cdna-driver] [Compiler-target translation][cdna-target]
The cited cache emitters do not branch on MEC firmware revision; they establish
the driver's selected sequence, not an independent minimum firmware version.
[Acquire emitter][cdna-linux-acquire] [Release emitter][cdna-linux-release]

### Acquire representation

GFX7–GFX9 MEC uses seven DWORDs for `ACQUIRE_MEM`, opcode `0x58`, count 5.
Mesa also uses this form for GFX9 graphics; earlier graphics uses
`SURFACE_SYNC`. [Legacy engine selection][mesa-acquire]

| DWORD | Representation |
| --- | --- |
| 0 | Type-3 header; opcode `0x58`, count 5. |
| 1 | `CP_COHER_CNTL` in bits 0–30. The generic layout names bit 31 `ENGINE_SEL`; the cited compute emitters leave it clear. |
| 2–3 | Coherency size in 256-byte units, low word followed by high part. This is a unit count, not count-minus-one. The high-part conventions differ below. |
| 4–5 | Coherency base in 256-byte units; low 32 bits followed by a 24-bit high part in the Linux and ROCr helpers. |
| 6 | Poll interval in bits 0–15; Linux emits `0xa`, while ROCr's zero-initialized code-cache packet leaves it zero. |

ROCr's range builder encodes its code-allocation address shifted right by
eight and rounds its byte size upward to a 256-byte count.
[Linux layout and masks][cdna-acquire-fields] [ROCr units][cdna-rocr-fields]
[ROCr range builder][cdna-code-cache]

The corresponding acquire and release action bits occupy different positions:

| Cache control | `ACQUIRE_MEM` DWORD 1 | `RELEASE_MEM` DWORD 1 |
| --- | --- | --- |
| Instruction-cache action | `SH_ICACHE_ACTION_ENA`, bit 29 | — |
| Scalar-cache action / volatile action / writeback | `SH_KCACHE_ACTION_ENA`, 27; `SH_KCACHE_VOL_ACTION_ENA`, 28; `SH_KCACHE_WB_ACTION_ENA`, 30 | — |
| Vector L1 action / volatile action | `TCL1_ACTION_ENA`, 22; `TCL1_VOL_ACTION_ENA`, 15 | `TCL1_ACTION_EN`, 16; `TCL1_VOL_ACTION_EN`, 12 |
| TC/L2 action | `TC_ACTION_ENA`, 23 | `TC_ACTION_EN`, 17 |
| TC/L2 volatile action | — | `TC_VOL_ACTION_EN`, 13 |
| TC/L2 writeback | `TC_WB_ACTION_ENA`, 18 | `TC_WB_ACTION_EN`, 15 |
| Non-coherent action | `TC_NC_ACTION_ENA`, 3 | `TC_NC_ACTION_EN`, 19 |
| Write-combined action | `TC_WC_ACTION_ENA`, 4 | — |
| Metadata action | `TC_INV_METADATA_ACTION_ENA`, 5 | `TC_MD_ACTION_EN`, 21 |

The table names fields, not a set of universally interchangeable masks; a dash
means that no corresponding control is supplied by the cited macros for that
packet.
[Acquire controls][cdna-acquire-fields] [Release controls][cdna-release-fields]

Full-range callers differ. ROCr and Mesa emit low size `0xffffffff` and high
size `0xff`, while the GC9.4.3/4 Linux emitter writes the same low size and
high size `0xffffff`; all use zero base. Linux's header contains an 8-bit
generic high-size helper and a 24-bit `_VG10` helper. The generic definition
alone does not resolve the wider value in the exact native emitter. These
source-selected forms retain their caller and transport predicates.
[Linux acquire][cdna-linux-acquire] [Linux high-size variants][cdna-acquire-fields]
[ROCr full range][cdna-code-cache] [Mesa full range][mesa-acquire]

### GC9.4.3/4 scheduled release and acquire

Linux's broad compute acquire selects instruction cache, scalar cache, TC/L2,
vector L1 and TC writeback: `CP_COHER_CNTL = 0x28c40000`. Its full-range packet
appears before an IB only when `AMDGPU_IB_FLAG_EMIT_MEM_SYNC` is requested.
The operation makes caches ready for subsequent work; it does not by itself
establish completion of an independent producer.
[Acquire emitter][cdna-linux-acquire] [Wrapper and flag][cdna-wrapper-acquire]

The corresponding fence emitter uses eight-DWORD `RELEASE_MEM`, opcode
`0x49`, count 6, `CACHE_FLUSH_AND_INV_TS_EVENT`, and event index 5. Its default
cache actions are TCL1, TC, TC writeback and TC metadata (`0x238000` before
the event fields). `AMDGPU_FENCE_FLAG_TC_WB_ONLY` instead selects TC writeback
and TC_NC (`0x88000`). DWORD 2 selects 32-bit or 64-bit data, leaves the
destination at MC, and selects interrupt-after-write-confirmation when the
interrupt flag is present. DWORDs 3–4 contain the byte address, DWORDs 5–6 the
value, and DWORD 7 is zero. The address is aligned to four bytes for a
32-bit write and eight for a 64-bit write. Kernel fence emission supplies the
interrupt flag; the optional user fence has its own flag path.
[Native EOP emitter][cdna-linux-release] [Release fields][cdna-release-fields]
[Kernel fence owner][cdna-fence-owner] [User-fence path][cdna-wrapper-release]

A complete scheduled producer/consumer edge retains the native wrapper:

1. Prepare mapped payloads, commands and dependencies through the
   [scheduled submission protocol](publication.md#scheduled-drm-publication).
   Submit the producer with the release policy its memory and next observer
   require. The wrapper emits the EOP fence after the IB.
2. Establish the producer dependency before the consumer. When the wrapper
   requires pipeline synchronization, its compute-ring operation waits for
   that ring's last emitted fence sequence using `WAIT_REG_MEM`, before the
   requested acquire. This ring-local wait is distinct from an external
   producer's scheduler dependency.
3. Request the consumer's pre-IB cache synchronization, then execute its
   shader work. A release packet being present in the command stream is not
   a wait for its asynchronous EOP; the dependency and any required wait
   precede the consumer acquire.
4. Observe the consumer's completed fence before reading or replacing its
   payload. Commands, code, arguments and signal storage retain their
   respective [final-use boundaries](command-buffers.md). The host mapping
   and host acquire operation remain part of a CPU observation.

[Wrapper ordering][cdna-wrapper-acquire] [Ring-local wait][cdna-pipeline-wait]
[Terminal release][cdna-wrapper-release]

The last IB's `AMDGPU_IB_FLAG_TC_WB_NOT_INVALIDATE` selects the writeback-only
trailer. Mesa's Gallium submission context sets that flag for graphics and
compute, while requesting pre-IB synchronization only when its
`ib_caches_flush` policy is enabled. These are transport-owned choices; a KFD
user ring or an AQL vendor packet does not inherit this scheduled trailer.
[Trailer selection][cdna-wrapper-release] [Mesa flag producer][cdna-mesa-flags]

### Code, shader data, and observer scope

ROCr's ordinary loader path first completes code upload: either a synchronous
DMA copy or CPU copy with its PCIe write-combining flush. It then calls
`InvalidateCodeCaches`. For GFX9 that function selects I/K/TC/writeback
(`0x28840000`), without the broad Linux acquire's TCL1 bit, and submits the
packet through the utility AQL queue. `ExecutePM4` defaults to NONE/NONE packet
scopes; its internal completion signal is acquire-waited before return. This
is an explicit code-cache operation after upload, not a SYSTEM data-handoff
template. Code storage still follows its
[publication and final-use contract](../aql/dispatch.md#executable-publication-and-final-use).
[Upload owner][cdna-loader] [Code-cache builder][cdna-code-cache]
[Default scopes][cdna-execute-defaults] [Completion owner][cdna-execute-completion]

ROCr's shader-copy path instead uses NONE/NONE dependency barriers followed by
a SYSTEM/SYSTEM dispatch. Its DMA interface requires the caller to ensure
system-level coherent buffers, with each participating agent able to access
both buffers. A DMA engine may lie outside the sender's or receiver's
coherency domain; in general the sender performs a SYSTEM release and the
receiver a SYSTEM acquire. With
`AMD_OPT_FLUSH` enabling agent fences, CLR starts from AGENT/AGENT gfx942
dispatch headers and adjusts them according to its SYSTEM-fence state. Its
pending-work/dirty-fence/external-signal path in
`releaseGpuMemoryFence` emits a SYSTEM/SYSTEM barrier, with a CPU wait when
requested. These are AQL runtime scope choices, not published PM4 microcode
expansions of those scopes.
[Shader dependency][cdna-blit-dependency] [Shader dispatch][cdna-blit-dispatch]
[DMA contract][cdna-dma-contract] [CLR dispatch policy][cdna-clr-dispatch]
[CLR setting][cdna-clr-setting] [CLR header adjustment][cdna-clr-adjust]
[CLR terminal barrier][cdna-clr-terminal] [CLR barrier header][cdna-clr-header]

### Mapping, partitions, and caches

RADV's legacy L2 invalidation includes vector-L1 invalidation and, on GFX8+,
L2 writeback. Its writeback-only path uses TC_NC with the explicit premise
`MTYPE <= 1`, and emits vector-L1 invalidation separately. That premise
cannot be generalized to GC9.4.3/4 local memory.
[RADV combinations and premise][cdna-mesa-legacy]

KFD's DEFAULT mappings initially select NC. For GC9.4.3/4, Linux's subsequent
native policy distinguishes same-device, same-memory-partition VRAM from
remote VRAM; local memory defaults to RW, with module choices NC or CC.
UNCACHED selects UC; EXT_COHERENT selects local CC or nonlocal UC. Without
those flags, dGPU system memory is UC and nonlocal VRAM remains NC. The APU
and per-page NUMA paths have separate predicates. Snooping and host
cacheability are additional properties. These rules describe the default
KFD mapping path; explicit DRM VM memory-type requests have their own initial
state. [Mapping defaults][cdna-mapping-default]
[Native locality and MTYPE][cdna-mapping] [Initial VM memory type][cdna-vm-mtype]
[Partitioned staging](../recipes/local-memory.md#native-hbm-cache-policy)

LLVM's gfx942 model allows several L2 caches in one logical agent. Its shader
release/acquire sequences include L2 writeback or invalidation according to
scope and local/nonlocal access; scalar reads rely on dispatch-time
immutability, with compiler-managed scalar writeback for spills. These
shader/ABI rules are distinct from the PM4 packet fields. Linux associates
each cited compute ring with an XCC; those emitters do not describe an
all-XCC broadcast or the firmware expansion of AQL fences.
[gfx942 cache model][cdna-llvm-model] [SYSTEM shader acquire][cdna-llvm-acquire]
[SYSTEM shader release][cdna-llvm-release] [Native ring/XCC owner][cdna-ring-xcc]

An SDMA or CPU observer therefore needs the release, completion and acquire
operations of its actual access path. HDP handles a host-aperture boundary;
it does not substitute for shader TC maintenance. Conversely, an EOP control
write becoming visible does not by itself make an arbitrary CPU mapping
coherent. The complete [staged transfer](../recipes/local-memory.md) and
[CPU/GPU handoff](../recipes/host-device.md) retain these separate obligations.

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
[mesa-pws-destination]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/44cc4ca677a4752a10c14194289bde5a6468675e/src/amd/vulkan/radv_cmd_buffer.c#L7999-L8052
[mesa-pfp-destination]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/44cc4ca677a4752a10c14194289bde5a6468675e/src/amd/vulkan/radv_cmd_buffer.c#L7843-L7885
[mesa-pws-partition]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/44cc4ca677a4752a10c14194289bde5a6468675e/src/amd/vulkan/radv_cs.c#L118-L172
[mesa-pws-draw]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/44cc4ca677a4752a10c14194289bde5a6468675e/src/amd/vulkan/radv_cmd_buffer.c#L14275-L14293
[mesa-pws-mesh]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/44cc4ca677a4752a10c14194289bde5a6468675e/src/amd/vulkan/radv_cmd_buffer.c#L14384-L14395
[mesa-pws-compute]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/44cc4ca677a4752a10c14194289bde5a6468675e/src/amd/vulkan/radv_cmd_buffer.c#L15335-L15349
[mesa-pws-ray]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/44cc4ca677a4752a10c14194289bde5a6468675e/src/amd/vulkan/radv_cmd_buffer.c#L15407-L15414
[mesa-pws-finalize]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/44cc4ca677a4752a10c14194289bde5a6468675e/src/amd/vulkan/radv_cmd_buffer.c#L8936-L8948
[mesa-pws-resolve]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/44cc4ca677a4752a10c14194289bde5a6468675e/src/amd/vulkan/radv_cmd_buffer.c#L16212-L16244
[mesa-pws-pfp-tail]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/44cc4ca677a4752a10c14194289bde5a6468675e/src/amd/vulkan/radv_cs.c#L251-L260
[mesa-pws-builder]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/44cc4ca677a4752a10c14194289bde5a6468675e/src/amd/common/ac_cmdbuf_cp.c#L149-L177
[pal-split-acquire]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9AcquireReleaseBarrier.cpp#L1711-L1793
[pal-fence-owner]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfxCmdBuffer.cpp#L480-L501
[pal-event]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/inc/core/palCmdBuffer.h#L2818-L2862
[pal-poll]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/device.h#L1023
[pal-me-sizes]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_f32_me_pm4_packets.h#L140-L164
[mesa-buffer-access]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_cmd_buffer.c#L7917-L7954

[cdna-driver]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_discovery.c#L2725-L2739
[cdna-target]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdkfd/kfd_device.c#L341-L354
[cdna-acquire-fields]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/soc15d.h#L392-L423
[cdna-release-fields]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/soc15d.h#L312-L339
[cdna-linux-acquire]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gfx_v9_4_3.c#L3496-L3513
[cdna-linux-release]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gfx_v9_4_3.c#L2985-L3017
[cdna-pipeline-wait]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gfx_v9_4_3.c#L3019-L3027
[cdna-wrapper-acquire]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_ib.c#L208-L252
[cdna-wrapper-release]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_ib.c#L299-L326
[cdna-fence-owner]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_fence.c#L101-L118
[cdna-mesa-flags]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/gallium/winsys/amdgpu/drm/amdgpu_cs.cpp#L833-L854
[cdna-mesa-legacy]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_cs.c#L451-L498
[cdna-rocr-fields]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/inc/amd_gpu_pm4.h#L77-L85
[cdna-code-cache]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L3437-L3498
[cdna-loader]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_loader_context.cpp#L347-L371
[cdna-execute-defaults]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/inc/queue.h#L438-L444
[cdna-execute-completion]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_aql_queue.cpp#L1693-L1757
[cdna-blit-dependency]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_kernel.cpp#L682-L686
[cdna-blit-dispatch]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_kernel.cpp#L887-L911
[cdna-dma-contract]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/inc/hsa_ext_amd.h#L2090-L2112
[cdna-clr-dispatch]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/clr/rocclr/device/rocm/rocvirtual.cpp#L2407-L2441
[cdna-clr-setting]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/clr/rocclr/device/device.cpp#L1644-L1651
[cdna-clr-adjust]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/clr/rocclr/device/rocm/rocvirtual.cpp#L1624-L1653
[cdna-clr-terminal]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/clr/rocclr/device/rocm/rocvirtual.cpp#L2347-L2364
[cdna-clr-header]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/clr/rocclr/device/rocm/rocvirtual.cpp#L71-L89
[cdna-mapping-default]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_amdkfd_gpuvm.c#L513-L524
[cdna-mapping]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gmc_v9_0.c#L1102-L1157
[cdna-vm-mtype]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gmc_v9_0.c#L1173-L1199
[cdna-llvm-model]: https://github.com/llvm/llvm-project/blob/6e714c8d91116794cb699cdf80c26afe9cda3ef3/llvm/docs/AMDGPUUsage.rst#L11263-L11339
[cdna-llvm-acquire]: https://github.com/llvm/llvm-project/blob/6e714c8d91116794cb699cdf80c26afe9cda3ef3/llvm/docs/AMDGPUUsage.rst#L11560-L11585
[cdna-llvm-release]: https://github.com/llvm/llvm-project/blob/6e714c8d91116794cb699cdf80c26afe9cda3ef3/llvm/docs/AMDGPUUsage.rst#L12492-L12535
[cdna-ring-xcc]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gfx_v9_4_3.c#L903-L930
[pal12-clients]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12Barrier.cpp#L53-L78
[pal12-normalize]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12Barrier.cpp#L314-L373
[pal12-transition-history]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12Barrier.cpp#L378-L433
[pal12-split-source]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12Barrier.cpp#L1516-L1534
[pal12-split-destination]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12Barrier.cpp#L1610-L1624
[pal12-token-wait]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12Barrier.cpp#L1048-L1067
[pal12-acquire-tail]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12Barrier.cpp#L1100-L1119
[pal12-front-end]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12Barrier.cpp#L435-L465
[pal12-deferred-dma]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12Barrier.cpp#L713-L725
[pal12-acquire-dma]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12Barrier.cpp#L955-L985
[pal-metadata-layout]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9Image.cpp#L3391-L3409
[pal-metadata-finalize]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9Image.cpp#L918-L924
[pal-metadata-range]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9Image.cpp#L3317-L3332
[pal-metadata-predicates]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9Image.cpp#L3410-L3500
[pal-metadata-tail]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9Image.cpp#L3057-L3077
[pal-gfx11-identity]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/device.h#L2328-L2333
[pal-gfx103-identity]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/device.h#L2515-L2526
[pal-metadata-transition]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9AcquireReleaseBarrier.cpp#L379-L435
[pal-metadata-blit-history]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9AcquireReleaseBarrier.cpp#L219-L258
[pal-metadata-inputs]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9Barrier.h#L330-L355
[pal-metadata-blit]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9AcquireReleaseBarrier.cpp#L969-L982
