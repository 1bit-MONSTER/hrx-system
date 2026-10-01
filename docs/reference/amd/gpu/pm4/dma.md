# Command-processor DMA

DMA_DATA performs command-processor copies, fills and prefetches. It runs
through PM4, not an [SDMA queue](../sdma/README.md), and has different
completion controls from confirmed [COPY_DATA](memory-commands.md). The caller
owns both transfer ranges and command storage, publishes prior producers
before DMA reads, and joins DMA writes before applying consumer visibility
operations or releasing the storage.

## Representation and controls

The GFX10/GFX11 MEC DMA_DATA form is opcode `0x50`, seven DWORDs:

| Word | Fields |
| --- | --- |
| 0 | Type-3 header, count 5. |
| 1 | Source cache policy bits 14:13, destination selector 21:20, destination cache policy 26:25, source selector 30:29. Other MEC fields are reserved, including bit 31. |
| 2–3 | Source byte address low/high, or immediate data for the selected fill mode. |
| 4–5 | Destination byte address low/high. |
| 6 | Direct 26-bit byte count; SAS/DAS bits 26/27; source/destination increment controls 28/29; RAW_WAIT 30; DIS_WC 31. |

[MEC layout][p11-mec] [PAL builder][p11-builder]

The source/destination selectors distinguish address routes, immediate fill
and destination-nowhere. Ordinary copies advance byte addresses. Fill and
prefetch have different footprints; a zero-byte drain is not a data transfer.
The principal ordering controls express different obligations:

| Control | Role in the ordinary callers |
| --- | --- |
| RAW_WAIT | Orders this operation after preceding DMA operations, including copy-after-copy read dependencies. It does not alone publish this copy to a later shader or host. |
| CP_SYNC | A PFP/ME control that stalls the selected processor for DMA completion. The corresponding bit is reserved in the cited MEC layout. |
| DIS_WC | Disables write confirmation. PAL keeps it clear for asynchronous data copies whose later barrier must guarantee arrival at the selected destination. |

[PAL transfer contract][p11-dma-info] [RADV emitter][m-dma]

## MEC completion discrepancy and ordinary caller

Mesa's emitter states that MEC DMA is synchronous and sets CP_SYNC only for
graphics queues. PAL's compute copy instead uses `sync=false`, tracks active
CP blits, and describes them as asynchronous in its postamble. PAL drains them
with `BuildWaitDmaData`, which emits a zero-byte operation with `sync=true`
through a shared PFP-layout builder. The MEC layout reserves that bit. The
GFX12 PAL implementation retains the distinction.
[Mesa emission][m-dma] [PAL copy][p11-copy] [PAL postamble][p11-postamble]
[PAL builder][p11-builder] [PFP layout][p11-pfp] [MEC layout][p11-mec]
[PAL wait][p11-dma-wait] [GFX12 copy][p12-copy]
[GFX12 postamble][p12-postamble] [GFX12 wait][p12-dma-wait]
[GFX12 MEC layout][p12-mec]

Busy bookkeeping and comments do not, by themselves, prove asynchronous
hardware behavior. The contradictory descriptions do not authorize setting a
reserved MEC control. RADV supplies a complete ordinary compute dependency
without doing so:

1. Its buffer-copy planner chooses CP DMA for an eligible small, non-sparse
   copy on a compute queue.
2. The copy loop flushes pending producer caches, uses the appropriate L2
   source/destination route, and clears its logical final CP_DMA_SYNC flag.
   The stream remains marked DMA-busy.
3. A COPY-stage barrier calls the drain: DMA_DATA with zero source,
   destination, count and remaining body fields. Although requested through
   the logical sync flag, the MEC emitter leaves CP_SYNC clear.
4. The following compute dispatch emits pending consumer cache operations
   before reading the copied payload.

[Copy planner][m-meta] [Queue mapping][m-queue] [Copy and drain][m-dma-callers]
[COPY-stage handling][m-copy-stage] [Barrier call][m-barrier]
[Consumer preparation][m-consumer]

This is an in-command-buffer dependency before any kernel submission trailer.
The fill loop has a different policy: it retains final logical synchronization.
Command-buffer end also drains pending DMA. Those actual callers are more
specific evidence than the preparation helper's last-packet flag alone.
[Copy/fill distinction][m-dma-callers] [Command-buffer end][m-end]

PFP_SYNC_ME is a graphics-front-end edge, not a compute-ring operation.
PAL's command-storage postamble separately joins shader users and relies on
a KMD EOP for later cache publication. Neither service is implicit in a raw
MEC user ring. [Linux PFP distinction][l12-fence] [PAL retirement][p11-postamble]

## Combined release wait and firmware identity

PAL can combine a CP-DMA wait with RELEASE_MEM. Its defaults use the PFP
firmware **image version**, separately from the ME **feature version** used
for [CS_PARTIAL_FLUSH](memory-commands.md#compute-completion-and-firmware).
Linux exposes `ver` and `feature` separately, and PAL stores the former in
`pfpUcodeVersion`. [PAL query][p-firmware] [Linux query fields][l-firmware]

| PAL generation | Default combined-wait eligibility |
| --- | --- |
| GFX10.1/10.3 | Disabled by the generation predicate. |
| GFX11.0 | PFP image version at least 2150. |
| GFX11.5 | Excluded by the exact GFX11.0 predicate, regardless of the numeric version. |
| GFX12 | Separate implementation, PFP image version at least 2330. |

[GFX11 predicate][p11-settings] [GFX11 threshold][p11-threshold]
[GFX12 predicate][p12-settings] [GFX12 threshold][p12-threshold]

Both settings begin true; PAL modifies defaults and reads overrides afterward.
The builder consumes the resulting setting. This is firmware-informed runtime
policy, not an unoverrideable capability query.
[GFX11 default][p11-setting-default] [GFX12 default][p12-setting-default]
[Settings lifecycle][p-settings-life]

If a requested combined wait is unavailable, the compute caller emits an
explicit DMA wait before its EOP fence and equality wait. Cache work not
represented by the release follows in ACQUIRE_MEM. Split barriers can defer
DMA completion and cache work together: flushing before a writer finishes
allows it to dirty the destination again.
[GFX10/11 completion][p11-wait-eop] [GFX12 completion][p12-wait-eop]
[Split-barrier planning][p11-barrier]

## Counts, alignment and algorithm selection

The 26-bit byte count is direct, not length minus one. Its representation
capacity, driver chunk size and algorithm threshold are different quantities.

| Boundary | Source value or policy |
| --- | --- |
| Field capacity | `(1 << 26) - 1` bytes; field width alone is not an execution guarantee for every firmware. |
| PAL compute chunks | `1 << 25` bytes, a power-of-two split policy. |
| Mesa GFX10 chunks | 67,108,832 bytes: the field mask rounded down to 32-byte alignment. |
| Mesa GFX11+ chunks | 32,736 bytes: 32,767 rounded down to 32-byte alignment. The reason for this smaller cap is not supplied by the cited code. |
| PAL shader threshold | A region exceeding the client setting chooses a shader; its default is 64 KiB. |
| RADV shader threshold | Normally prefers a shader from 4096 bytes. On GFX10+ dedicated-VRAM devices, non-device-local copies may use CP DMA through 65,536 bytes. Alignment and sparse-resource requirements also affect selection. |

[Field layout][p11-mec] [PAL older limit][p11-dma-limit]
[PAL GFX12 limit][p12-dma-limit] [PAL copies][p11-copy]
[GFX12 copies][p12-copy] [Mesa cap][m-dma]
[PAL algorithm][p-copy-select] [Default threshold][p-copy-default]
[RADV algorithm][m-meta] [RADV thresholds][m-thresholds]

RADV can choose CP DMA for an unaligned copy; 32-byte alignment is a performance
preference. Fill requires a DWORD-aligned destination and length and repeats a
32-bit value. The older source/counter-alignment repair is restricted to
families through Carrizo or Stoney; it is not the GFX10/11/12 rule.
[Actual transfer loops][m-dma-callers] [Operation selection][m-meta]

Prefetch has a different access envelope. RADV caps a GFX11+ request, rounds the
fetched interval outward to 32-byte boundaries and disables write confirmation.
PAL also uses destination-nowhere and disabled confirmation. The prefetched
backing must cover the enlarged read interval; these choices do not authorize
enlarging an ordinary copy beyond its source or target range.
[RADV prefetch][m-prefetch] [PAL prefetch][p12-prefetch]

## Cache routes and architecture differences

GFX10/GFX11 ordinary CP copies generally route through L2. On GFX12, the same
TC_L2 selector names route through MALL and bypass shader GL2. PAL's copy marks
shader L2 stale and its planner distinguishes both handoff directions:

| Handoff | GFX12 PAL cache action |
| --- | --- |
| Shader GL2 writer → CP reader | Write back GL2 so the bypass path sees the payload. |
| CP writer → shader GL2 reader | Invalidate GL2 and write it back to preserve other valid dirty data. |

[PAL GFX12 copy][p12-copy] [Cache planner][p12-cache]
[Client classes][p12-cache-actors] [Mesa route][m-dma]

The [cache-client planner](cache.md#gfx12-cp-and-shader-handoffs) accounts for
missing split-barrier information and retained dirty data from earlier writers.
Its deferred CP-DMA token path keeps cache work after the DMA join. Mesa's
INV_L2 likewise lowers to writeback plus invalidation. Transfer completion
or a route name alone does not establish CPU visibility or fresh shader
scalar/vector caches. [Mesa cache actions][m-cache]

Mesa selects `cp_sdma_ge_use_system_memory_scope` for exact GFX12. The cited
RADV revision rejects later generations, so this predicate is not a GFX12.1
recipe. Mesa also excludes CP DMA on compute-only GFX940+ devices. A compute
queue on a graphics-capable RDNA device is a different condition from a
compute-only CDNA device. [Common device properties][m-info]
[RADV generation boundary][m-admission]

Source/destination mappings and all command references remain live through
DMA completion and the final dependent consumer. Shader completion, DMA
completion, payload cache release and command retirement are separate edges;
PAL's MEC control discrepancy does not disappear by substituting one for
another.

[p11-mec]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_f32_mec_pm4_packets.h#L1315-L1387
[p11-builder]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9CmdUtil.cpp#L2225-L2320
[p11-dma-info]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9CmdUtil.h#L149-L178
[m-dma]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_cp_dma.c#L20-L127
[p11-copy]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9ComputeCmdBuffer.cpp#L1738-L1770
[p11-postamble]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9ComputeCmdBuffer.cpp#L1228-L1263
[p11-pfp]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_f32_pfp_pm4_packets.h#L1520-L1595
[p11-dma-wait]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9CmdUtil.cpp#L4339-L4357
[p12-copy]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12ComputeCmdBuffer.cpp#L1608-L1640
[p12-postamble]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12ComputeCmdBuffer.cpp#L947-L979
[p12-dma-wait]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12CmdUtil.cpp#L2481-L2499
[p12-mec]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/chip/gfx12_merged_f32_mec_pm4_packets.h#L1163-L1235
[m-meta]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/meta/radv_meta_buffer.c#L233-L391
[m-queue]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_queue.c#L2095-L2104
[m-dma-callers]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_cp_dma.c#L202-L391
[m-copy-stage]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_cmd_buffer.c#L1708-L1720
[m-barrier]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_cmd_buffer.c#L16384-L16399
[m-consumer]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_cmd_buffer.c#L15321-L15384
[m-end]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_cmd_buffer.c#L8887-L8902
[l12-fence]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/gfx_v12_0.c#L4565-L4627
[p-firmware]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/os/amdgpu/amdgpuDevice.cpp#L933-L985
[l-firmware]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/amdgpu_kms.c#L237-L244
[p11-settings]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9SettingsLoader.cpp#L855-L867
[p11-threshold]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9Chip.h#L729-L730
[p12-settings]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12SettingsLoader.cpp#L165-L191
[p12-threshold]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12SettingsLoader.cpp#L45-L49
[p11-setting-default]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/settings_gfx9.json#L117-L126
[p12-setting-default]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/settings_gfx12.json#L343-L352
[p-settings-life]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfxDevice.cpp#L128-L172
[p11-wait-eop]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9ComputeCmdBuffer.cpp#L1780-L1847
[p12-wait-eop]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12ComputeCmdBuffer.cpp#L1721-L1785
[p11-barrier]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9AcquireReleaseBarrier.cpp#L1330-L1395
[p11-dma-limit]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9CmdUtil.h#L297-L299
[p12-dma-limit]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12CmdUtil.h#L218-L220
[p-copy-select]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/rpm/rsrcProcMgr.cpp#L5524-L5563
[p-copy-default]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/device.cpp#L539-L545
[m-thresholds]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_constants.h#L45-L55
[m-prefetch]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_cp_dma.c#L132-L187
[p12-prefetch]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12CmdUtil.cpp#L2851-L2870
[p12-cache]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12Barrier.cpp#L375-L432
[p12-cache-actors]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx12/gfx12Barrier.cpp#L52-L75
[m-cache]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_cs.c#L74-L112
[m-info]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/common/ac_gpu_info.c#L1152-L1218
[m-admission]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_physical_device.c#L2501-L2513
