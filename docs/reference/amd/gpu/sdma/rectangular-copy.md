# SDMA rectangular linear-buffer copy

`COPY_LINEAR_SUBWIN` copies a rectangular volume between two linear address
spaces with independent row and slice pitches. SDMA transfers the elements in
each selected row and leaves the gaps between rows and slices untouched. The
ordinary form uses 13 DWORDs, COPY opcode 1 and suboperation 4. ROCr calls this
form `COPY_LINEAR_RECT`; PAL uses it for typed buffers and linear images.
Neither operation performs format conversion. [ROCr builder][rocr-build]
[PAL caller][pal-caller] [PAL typed-buffer contract][pal-api]

## Callers and applicability

ROCr's `hsa_amd_memory_async_copy_rect` accepts byte-based pitched memory:
X offsets and width are bytes, Y is rows, and Z is layers. Both rectangles
must be directly accessible to the selected GPU agent and must not overlap.
The entry point rejects a CPU copy agent and host-to-host direction. Its GPU
implementation requires ISA major 9 or later and an available SDMA blit
engine; it returns an error rather than substituting a shader copy.
[Public contract][rocr-api] [Entry point][rocr-entry]
[GPU selection][rocr-agent]

PAL's DMA command-buffer path accepts typed-buffer regions: GPU allocation
plus byte offset, byte row/depth pitches, format, and an extent in pixels or
compression blocks. It converts these quantities before calling the GFX10
or GFX12 `WriteCopyTypedBuffer` builder. PAL's GFX10 implementation also serves
its GFX11 device path. RADV reaches the ordinary packet when copying between
linear surfaces, or between a buffer and a linear image; tiled surfaces use
different suboperations. [PAL conversion][pal-convert]
[GFX10 builder][pal10-build] [GFX12 builder][pal12-build]
[PAL implementation include][pal-dma-include]
[PAL DMA selection][pal-dma-selection] [RADV buffer/image dispatch][radv-caller]
[RADV image dispatch][radv-image]

PAL also excludes overlapping destination regions and source/destination
overlap within the same allocation. These interfaces supply copy semantics,
not `memmove` semantics; subdivision or a drain does not make overlap valid.
[PAL overlap contract][pal-api] [Ordering and overlap](ordering.md#overlap-and-ownership)

These are consumer-specific architecture predicates. A compiler GFX target,
a native SDMA revision, and a firmware version are separate inputs. The
rectangular builders cited here contain no rectangular-specific firmware
version threshold; their queue, dependency and cache policies still apply.

## Addresses, elements and the touched extent

Let `E = 2^elementsize` bytes. ROCr selects `E` from 1, 2, 4, 8 or 16, encoded
as 0 through 4 in the three-bit field. The field width alone does not establish
the meaning of encodings 5 through 7. Row and slice pitches in the packet are
element counts, not byte counts. X offsets and width also count elements;
Y offsets/heights count rows and Z offsets/depths count layers.
[Element selection and limits][rocr-elements]

For packet base address `B`, offsets `(x, y, z)`, decoded positive row pitch
`P`, slice pitch `S`, and an element `(i, j, k)` within the copy, its byte
address is:

```text
B + E * ((x + i) + (y + j) * P + (z + k) * S)
```

The source and destination apply that calculation independently with their
own base, offsets and pitches. Offsets are direct coordinates. Pitches and
positive copy extents use **count minus one** in the ordinary modern form.
There is no aggregate byte-count operand. [ROCr field writes][rocr-emit]
[PAL field writes][pal10-build]

For the byte-based ROCr input, a positive `W × H × D` request touches `W`
bytes in each of `H` rows and `D` slices. Its final addressed byte lies before

```text
base + offset_x + (offset_y + H - 1) * pitch
     + (offset_z + D - 1) * slice + W
```

on each side. The payload length `W * H * D` excludes pitch gaps and therefore
does not describe the required backing extent. ROCr checks that X plus width
fits the row, and, when a nonzero slice is supplied, that Y plus height fits
the rows per slice. The memory owner supplies the accessible allocation
extent; the packet contains no allocation bound. [Geometry checks][rocr-checks]
[Buffer contract][rocr-copy-contract]

## Ordinary 13-DWORD representation

The table separates ROCr's common pre-GFX12 definition, the wider-Z PAL
GFX10/Linux SDMA6 definition, and the GFX12 geometry used by PAL and ROCr.
Addresses are full byte addresses split low word first. Reserved bits are
zero in the ordinary builders. Control fields outside the geometry are
listed separately below. [ROCr common layout][rocr-layout]
[PAL GFX10 layout][pal10-layout] [Linux SDMA6 layout][linux6-layout]
[ROCr GFX12 layout][rocr12-layout] [PAL GFX12 layout][pal12-layout]

| DWORD | Quantity and unit | ROCr common pre-GFX12 | PAL GFX10 / Linux SDMA6 | PAL/ROCr GFX12 geometry |
| --- | --- | --- | --- | --- |
| 0 | Header | Opcode 7:0 = 1; suboperation 15:8 = 4; element-size logarithm 31:29. | Same positions. | Same positions. |
| 1, 2 | Source byte address | Low/high 32 bits. | Same. | Same. |
| 3 | Source X/Y offsets | X 13:0; Y 29:16. | Same. | X 15:0; Y 31:16. |
| 4 | Source Z offset; row pitch minus one | Z 10:0; pitch 31:13. | Z 12:0; pitch 31:13. | Z 13:0; pitch 31:16. |
| 5 | Source slice pitch minus one | 27:0. | Same. | 31:0. |
| 6, 7 | Destination byte address | Low/high 32 bits. | Same. | Same. |
| 8 | Destination X/Y offsets | X 13:0; Y 29:16. | Same. | X 15:0; Y 31:16. |
| 9 | Destination Z offset; row pitch minus one | Z 10:0; pitch 31:13. | Z 12:0; pitch 31:13. | Z 13:0; pitch 31:16. |
| 10 | Destination slice pitch minus one | 27:0. | Same. | 31:0. |
| 11 | Width/height minus one | Width 13:0; height 29:16. | Same. | Width 15:0; height 31:16. |
| 12 | Depth minus one | 10:0. | 12:0. | 13:0. |

### Header and memory-policy fields

| Source layout | Additional fields | Ordinary producer behavior |
| --- | --- | --- |
| PAL GFX10 | DWORD 0 TMZ bit 18; GFX10.3+ CPV bit 19. DWORD 12 destination swap 17:16 and source swap 25:24; GFX10.3+ destination cache policy 20:18 and source cache policy 28:26. | TMZ follows protected source memory. Swap fields remain zero. Cache policies are populated when `supportsMall`; CPV depends on the setting and valid KMD-provided policy. |
| ROCr common pre-GFX12 | DWORD 12 destination swap 17:16 and source swap 25:24; the remaining non-geometric fields are reserved in this definition. | Zero; the builder does not set scope or NPD in this branch. |
| PAL GFX12 | DWORD 0 TMZ bit 18. DWORD 12 destination MALL policy 21:20 and source MALL policy 29:28. | TMZ follows the copy flag; MALL policy follows source/destination settings when MALL exists, otherwise zero. |
| ROCr GFX12 | DWORD 0 NPD bit 28. The `gfx12` view has destination cache policy 22:20 and source cache policy 30:28. | Unscoped packets leave these controls zero. |
| ROCr `gfx1250` view | DWORD 12 destination scope 19:18, destination temporal hint 22:20, source scope 27:26, source temporal hint 30:28. | The scoped GFX12 branch sets NPD and both SYS scopes (3), leaving temporal hints zero. |

[PAL cache-policy selection][pal10-policy] [PAL MALL selection][pal12-policy]
[ROCr branch and scope writes][rocr-emit]

The different field names and widths are tied to these layouts. In particular,
PAL's two-bit MALL policy and ROCr's three-bit cache-policy/temporal-hint view
are not one interchangeable field definition. Cache placement hints also do
not replace a producer release, consumer acquire, or copy-completion operation.
The surrounding [cache protocol](cache.md) supplies those obligations.

## Encoded limits and caller limits

An N-bit count-minus-one field represents positive counts through `2^N`.
An N-bit direct offset instead represents `0..2^N-1`. Byte widths and pitches
are the element counts below multiplied by `E`; height and depth have no
element-size multiplier.

| Layout or producer | Row pitch, elements | Slice pitch, elements | Width, elements | Height, rows | Depth, layers |
| --- | --- | --- | --- | --- | --- |
| ROCr common layout and pre-GFX12 tile caps | `2^19` | `2^28` | `2^14` | `2^14` | `2^11` |
| PAL GFX10 / Linux SDMA6 field capacity | `2^19` | `2^28` | `2^14` | `2^14` | `2^13` |
| PAL/ROCr GFX12 field capacity; ROCr GFX12 tile caps | `2^16` | `2^32` | `2^16` | `2^16` | `2^14` |

[Definitions][rocr-layout] [GFX12 definitions][rocr12-layout]
[Runtime caps][rocr-elements]

Mesa documents a native transition from a 14-bit row pitch at bit 16 on
SDMA2.4, to a 19-bit pitch at bit 13 on SDMA4.0, and a Z expansion from 11
to 13 bits on SDMA5.0. Its emitter selects pitch bit 16 again at SDMA7.0+.
The actual Mesa helper limits row pitch to `2^14` elements below SDMA7.0 and
`2^16` thereafter, even where the field can represent more. It requires a
positive pitch with byte alignment equivalent to a DWORD. The ordinary
linear-subwindow path calls this helper with `uses_depth=false`, so the
helper's separate slice-pitch assertions are not checks performed by that
caller. This emitter does not tile the request. [Mesa emitter and checks][mesa-build]

The geometry differences have two source boundaries:

- ROCr retains the 11-bit common Z limit before GFX12. PAL's GFX10 packed
  definition and Linux SDMA6 macros expose 13 bits. PAL's builder comment
  still says 11-bit depth; its GFX12 comment also retains the old 14/11-bit
  dimensions. Those comments do not match their packet definitions.
- Linux's pinned `sdma_v7_1_0_pkt_open.h` retains the older 19-bit pitch,
  14-bit X/Y and 13-bit Z subwindow fields. PAL/ROCr's GFX12 definitions and
  Mesa's SDMA7.0+ pitch placement differ. The Linux header alone supplies no
  ordinary rectangular producer that resolves this discrepancy.

[PAL builders][pal10-build] [GFX12 builder][pal12-build]
[Linux SDMA7.1 definition][linux71-layout]

Mesa also has an explicit SDMA2.0 branch that writes direct width, height and
depth rather than subtracting one. That older encoding is separate from the
count-minus-one forms above. [Version branch][mesa-build]

## Alignment, subdivision and empty work

ROCr requires DWORD-aligned base addresses, byte row pitches and byte slice
pitches. Byte X offsets and widths need not be DWORD-aligned. For each tile,
the builder rebases the source and destination to DWORD-aligned addresses and
encodes their residual X displacement in elements. Y and Z offsets become
part of the rebased addresses, leaving their packet coordinates zero.
[Checks][rocr-checks] [Rebasing][rocr-rebase]

Element selection must exactly divide both row pitches and, for a multislice
copy, both slice pitches. It must also describe the tile width and the residual
source/destination X displacements. ROCr checks pitch capacity at the smallest
element size needed anywhere in the request, then tiles X, Y and Z within its
chosen layout's limits. A wider element in one tile cannot justify truncating
the pitches in a smaller-element tail. [Element constraints][rocr-elements]
[Tiling][rocr-rebase]

For a two-dimensional request whose byte row pitch exceeds the layout's
numeric element-pitch limit, ROCr first rotates the copy into the X–Z plane:
the original row pitch becomes the slice pitch, height becomes depth, and
height is set to one. It absorbs the original Y/Z offsets into the base.
This uses the wider slice field; the later element and slice-capacity checks
still apply. It does not select the LARGE opcode. [Wide-pitch conversion][rocr-wide]

Unused strides have explicit producer choices. When the request passed to the
rectangular builder has one layer, ROCr writes a zero slice-pitch field, since
no slice step occurs. A one-layer tail tile of a multislice request retains
the request's slice pitch. The X–Z conversion
sets the internal row pitch to zero; subtracting one fills that field with
ones, but the tile has one row and a zero Y offset. Neither choice gives a
general zero-stride interpretation to a count-minus-one field.
[Conversion][rocr-wide] [Field writes][rocr-emit]

PAL converts byte pitches by dividing by the element size and requires exact
divisibility. A 12-byte `R32G32B32` texel becomes three four-byte elements,
tripling the encoded width. Its typed-buffer loop emits one subwindow packet
per region without ROCr-style tiling or field-capacity checks. Regions must
therefore already fit the selected representation. PAL's public offset rule
is alignment to the smaller of the texel size and four bytes; its builder
uses that offset directly in the base address. This public rule is broader
than ROCr's DWORD-base check for one- and two-byte texels. The cited paths do
not establish a common relaxation of that check across their transports.
[PAL conversion][pal-convert] [PAL region loop][pal-caller]
[PAL alignment contract][pal-api]

The count-minus-one form has no empty-rectangle encoding. ROCr returns success
without submitting work when any input dimension is zero; that path neither
waits dependencies nor decrements the output signal. Positive rectangles
proceed to its geometry checks and builder. PAL's builders subtract one from
the supplied extents without an empty-region branch. [ROCr empty path][rocr-entry]
[PAL extent writes][pal10-build]

All ROCr tiles are assembled into one submission. Their command bytes and
dependency/cache/completion envelope must fit an available contiguous ring
extent; `AcquireWriteAddress` rejects a request as large as the ring. Thus
tiling removes individual-packet extent limits, not every runtime resource
limit. [Submission][rocr-submit-rect] [Ring reservation][rocr-reserve]

## Publication, dependency and final use

The ordinary ROCr flow connects the geometry to the memory and queue owners:

1. The owner provides nonoverlapping, directly accessible pitched storage and
   retains every addressed row. The sending device releases input data to
   SYSTEM scope as required by the async-copy contract. Dependency signals
   describe completed producers; all must reach zero before the copy begins.
2. ROCr validates and encodes the rectangles. `SubmitCommand` copies those
   packet bytes into its reserved native SDMA ring. On the selected gfx90x
   software-poll path, a callback-owned copy retains them until dependencies
   permit submission. The temporary packet vector is not device-borrowed
   indirect storage.
3. The published stream waits dependencies, performs the selected HDP/cache
   acquire, executes every rectangular tile, and performs the selected cache
   release. Completed packet bytes precede write-pointer and release-ordered
   doorbell publication.
4. ROCr then updates the output signal using its platform-selected atomic
   decrement or FENCE sequence. A successful call reports submission; the
   completion value reports the asynchronous result. The receiving device
   performs the SYSTEM acquire required before consuming the destination.
5. Source storage can be reused after its final copy read completes.
   Destination storage remains live through its last downstream consumer.
   Signals and any mailbox remain live through their final protocol users;
   the optional mailbox FENCE/TRAP follows the data-completion update. Ring
   retirement is a separate command-storage boundary.

[Public memory/dependency contract][rocr-copy-contract]
[Dependency handling][rocr-dependencies] [Deferred command ownership][rocr-callback]
[Stream composition][rocr-stream] [Publication][rocr-publish]
[Completion and notification tail][rocr-complete]

ROCr creates and owns the native SDMA queue and its ring allocation. Destruction
destroys the native queue before releasing the ring. The [signal and command
retirement protocols](atomics.md#memory-and-lifetime) explain why observing a
payload-completion value does not by itself retire later notification work or
another queue's references. [Queue construction][rocr-queue]
[Queue destruction][rocr-destroy]

PAL expresses typed-buffer hazards with `PipelineStageBlt`, `CoherCopySrc`
and `CoherCopyDst`. A later operation consuming the destination needs the
appropriate [transfer drain](ordering.md#pending-transfer-drains) and cache
transition. RADV's unaligned buffer/image path makes this concrete: it copies
through temporary storage using linear copies and subwindow copies, inserting
NOP drains before the next stage consumes or reuses that storage. The
subwindow packet alone is not a completion fence. [PAL contract][pal-api]
[RADV temporary-storage sequence][radv-unaligned]

### Rectangular-specific scope boundary

The rectangular producer must be read together with ROCr's template selector.
Outside DXG, major 9 selects no GCR or packet scope fields; major 10 and
major 11/12 with minor below 5 select USER_GCR. Major 11/12 with minor at least
5 select the scoped template without GCR. DXG selects the unscoped template
and attributes surrounding GCR work to its driver. [Factory][rocr-factory]
[Template definitions][rocr-templates]

Within `BuildCopyRectCommand`, however, NPD and SYS scope writes occur only
in the GFX12-or-later packet branch. The pre-GFX12 rectangular branch does
not use `scopeFields`. Consequently the non-DXG gfx11.5 selection combines
no USER_GCR with no rectangular-packet scope writes. The cited sources do not
explain an equivalent payload-visibility mechanism for that combination.
The scoped [one-dimensional copy](copy.md) cannot supply that missing
rectangular behavior, and a kernel submission's cache wrapper belongs to its
own transport. [Exact rectangular branches][rocr-emit]
[Conditional GCR emission][rocr-stream] [Transport ownership](cache.md)

## LARGE and compressed representations

`COPY_LINEAR_SUBWIN_LARGE` is a distinct suboperation, 36, with 20 DWORDs in
PAL's GFX10 and GFX12 definitions. Coordinates and row pitches occupy full
DWORDs, slice pitches occupy 48 bits, and the three extent fields occupy
DWORDs 17–19. Its header has no ordinary `elementsize` field. Neither the
ROCr rectangular tiler nor PAL's typed-buffer builders above selects this
form. The definitions establish its different storage layout, not a
drop-in conversion of the ordinary element units or a complete LARGE caller
protocol. [LARGE layouts][pal10-large] [GFX12 LARGE layout][pal12-large]
[Suboperation values][pal-subops]

A block-compressed texture format is a separate issue: PAL measures its
typed-buffer extent in compression blocks and copies those blocks without
format conversion. That contract does not select an SDMA metadata-compression
operation. PAL's ordinary GFX12 typed-buffer builder remains 13 DWORDs and
does not call `SetupMetaData`, even though its common caller can carry
compressed-source/destination flags. Its one-dimensional GFX12 COPY_LINEAR
builder has a separate conditional metadata DWORD. RADV's tiled-subwindow
path likewise has explicit DCC selection and generation-specific metadata
operands. Those metadata protocols do not follow from the ordinary linear
subwindow header or its element size. [Typed-buffer contract][pal-api]
[Common flags][pal-caller] [GFX12 typed builder][pal12-build]
[GFX12 linear metadata][pal12-linear-meta] [RADV tiled metadata][mesa-tiled-meta]

[rocr-api]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/inc/hsa_ext_amd.h#L2466-L2501
[rocr-copy-contract]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/inc/hsa_ext_amd.h#L2090-L2136
[rocr-entry]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/hsa_ext_amd.cpp#L804-L847
[rocr-agent]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L2234-L2261
[rocr-factory]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L852-L903
[rocr-templates]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/inc/amd_blit_sdma.h#L579-L590
[rocr-layout]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/inc/sdma_registers.h#L313-L433
[rocr12-layout]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/inc/sdma_registers.h#L435-L559
[rocr-checks]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L1859-L1880
[rocr-wide]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L1882-L1923
[rocr-submit-rect]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L1925-L1931
[rocr-build]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L2445-L2603
[rocr-elements]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L2445-L2512
[rocr-rebase]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L2514-L2547
[rocr-emit]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L2549-L2603
[rocr-queue]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L220-L245
[rocr-destroy]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L282-L306
[rocr-callback]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L308-L362
[rocr-dependencies]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L397-L443
[rocr-stream]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L521-L578
[rocr-complete]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L614-L650
[rocr-reserve]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L1953-L1988
[rocr-publish]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L1997-L2056
[pal-api]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/inc/core/palCmdBuffer.h#L3394-L3417
[pal-caller]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/dmaCmdBuffer.cpp#L534-L592
[pal-convert]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/dmaCmdBuffer.cpp#L1387-L1429
[pal-dma-include]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9Device.cpp#L63
[pal-dma-selection]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9Device.cpp#L2935-L2948
[pal10-build]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx10/gfx10DmaCmdBuffer.cpp#L526-L595
[pal12-build]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx12/gfx12DmaCmdBuffer.cpp#L456-L503
[pal10-policy]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx10/gfx10DmaCmdBuffer.cpp#L386-L450
[pal12-policy]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx12/gfx12DmaCmdBuffer.cpp#L345-L385
[pal10-layout]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx10/gfx10_merged_sdma_packets.h#L848-L1004
[pal12-layout]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx12/gfx12_merged_sdma_packets.h#L804-L940
[pal10-large]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx10/gfx10_merged_sdma_packets.h#L1006-L1200
[pal12-large]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx12/gfx12_merged_sdma_packets.h#L942-L1134
[pal-subops]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx12/gfx12_merged_sdma_packets.h#L59-L68
[pal12-linear-meta]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx12/gfx12DmaCmdBuffer.cpp#L416-L453
[linux6-layout]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v6_0_0_pkt_open.h#L753-L923
[linux71-layout]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v7_1_0_pkt_open.h#L753-L923
[mesa-build]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/common/ac_cmdbuf_sdma.c#L129-L188
[mesa-tiled-meta]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/common/ac_cmdbuf_sdma.c#L324-L383
[radv-caller]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_sdma.c#L248-L297
[radv-image]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_sdma.c#L385-L403
[radv-unaligned]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_sdma.c#L316-L382
