# SDMA ordering and completion

Packet order alone does not establish completion of dependent transfers.
ROCr's ordinary copy path explicitly allows serial COPY/WRITE operations to
overlap and selects FENCE, rather than WRITE, for completion publication. PAL
and RADV insert an explicit drain at transfer dependency boundaries. These
source contracts distinguish execution ordering from the separate [cache
visibility](cache.md) and storage lifetime obligations. [ROCr completion
selection][rocr-completion]

## Pending-transfer drains

The ordinary Mesa `ac_emit_sdma_nop` emits one zero DWORD and documents that
it waits for pending copies. RADV's transfer-queue barrier reaches that exact
helper through `radv_sdma_emit_nop`. Its comment treats automatic GFX9+ RAW
tracking as insufficient, without identifying a repaired revision or firmware
threshold. This is a conservative consumer policy, not an all-device erratum
or evidence that a particular omitted drain fails. [Mesa emitter][mesa-nop],
[RADV wrapper][radv-nop], [barrier caller][radv-barrier]

PAL independently supplies an all-image-types hazard mask in its GFX10 and
GFX12 DMA constructors. The shared `CmdReleaseThenAcquire` path then emits one
NOP when the combined source-stage mask is nonzero. That mask includes global
and memory barriers, so the path applies to buffer dependencies even without
an image barrier. Image transitions can require an additional drain after
their own transfer work. The older generic comment claiming strict in-order
execution does not describe these operative conditions.
[Constructors][pal-constructor], [GFX12 constructor][pal12-constructor],
[barrier implementation][pal-barrier]

For PAL's OSS4+ NOP representation, the count denotes body DWORDs excluding
the header. The classic GFX10/GFX11 header has opcode in bits 7:0,
suboperation in 15:8, count in 29:16 and two reserved bits. A one-DWORD NOP
has no body and encodes as zero, agreeing with Mesa. A larger padding packet's
count describes its storage extent; it is not a transfer count or a delay.
[PAL builder][pal-nop], [header fields][pal-nop-fields]

The source composition for a dependent copy is:

```text
COPY A -> B
NOP                       complete pending transfers before consuming B
COPY B -> C
FENCE completion          publish final completion to its owned word
```

Each copy has nonoverlapping source and destination ranges. The dependency is
between the first packet's destination and the second packet's source. The NOP
carries no address, value, cache-operation mask or signal. It does not by
itself notify the host, acquire another engine's payload, or select the memory
mapping and cache policy needed by this composition.

## Completion operations have different results

| Operation | Source contract and observation boundary |
| --- | --- |
| COPY/WRITE | Ordinary transfer work can overlap. A trailing WRITE is not a substitute for a completion fence. |
| NOP | Joins pending transfer work at an in-stream dependency boundary; it produces no memory result for a host waiter. |
| FENCE32 | Orders the preceding transfers and writes a known DWORD. The memory-type/scope fields and host observation follow the selected [fence contract](fence.md). A single DWORD store is not an atomic 64-bit signal update. |
| GET_GLOBAL timestamp | PAL specifies completion of preceding commands before recording the clock. The timestamp write still has its own visibility and final-use boundary; a later completion marker can establish that the timestamp destination is ready to read. |
| Memory poll | Waits for the selected operand predicate. Signal ownership and payload visibility remain the separate [poll](poll.md) and cache contracts. |

[PAL timestamp ordering][pal-timestamp] supplies a second way to separate
transfers when a clock sample is also needed. The recorded interval includes
that ordering point. It differs from timing an unseparated stream in which
transfers can overlap.

## Overlap and ownership

Overlap within one copy and dependency between distinct copies are separate
contracts. ROCr's public asynchronous-copy API explicitly excludes overlapping
source/destination ranges: submission can succeed while destination contents
remain undefined. Neither an inserted drain nor choosing a forward/reverse
chunk order establishes `memmove` behavior for overlapping ranges. [ROCr API
precondition][rocr-overlap]

The producer owns the input bytes through their final device read. Each
intermediate allocation remains live until its last dependent consumer has
completed, and completion storage remains live through its final writer and
waiter. The ring's acquired consumed frontier only retires command bytes; it
does not establish completion of referenced transfers. A fence placed before a
later consumer cannot authorize release of that consumer's input. ROCr's [ring
publication and reuse][rocr-ring] and its [completion
sequence](atomics.md#memory-and-lifetime) have separate owners for these
boundaries.

## Architecture and transport differences

PAL's GFX11.5 device selection reaches the DMA constructor and barrier path
above. Its pending-transfer drain therefore has an actual generation-specific
caller, rather than only a matching NOP bit pattern. [PAL
factory][pal-factory] [GFX11.5 selection][pal-asic]

RADV's supported-device predicate excludes compute-only GFX9 devices. Its
barrier behavior consequently does not describe CDNA SDMA4.4.2. Likewise,
Linux's use of NOP for ring padding establishes that padding representation,
not by itself the pending-transfer dependency semantics used by PAL and RADV.
[RADV device selection][radv-admission]

## Scope and prior dependency

ROCr names COPY_LINEAR header bit 28 NPD, “no prior dependency.” The scoped
copy builder sets NPD and SYS source/destination scopes. Linux's OSS7.1 header
agrees on the NPD position, but its ordinary copy emitter leaves that bit
zero. The ROCr factory selects the scoped V6 template for non-DXG ISA major
11/12 with minor at least 5, while its DXG path selects the unscoped V4
template. [Template meaning][npd-name] [Copy builder][npd-builder] [Linux
field][npd-layout] [Linux copy][npd-zero] [Factory][npd-transport]

These choices identify the emitted policies; the cited definitions do not
explain the complete prior-dependency domain that NPD bypasses. NPD is not a
prefetch-control bit, a completion marker, or a substitute for an explicit
producer/consumer dependency.

[rocr-completion]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L469-L479
[mesa-nop]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/common/ac_cmdbuf_sdma.c#L15-L22
[radv-nop]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_sdma.c#L210-L215
[radv-barrier]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_cmd_buffer.c#L16383-L16397
[pal-constructor]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx10/gfx10DmaCmdBuffer.cpp#L50-L66
[pal12-constructor]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx12/gfx12DmaCmdBuffer.cpp#L68-L85
[pal-barrier]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/dmaCmdBuffer.cpp#L387-L447
[pal-nop]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx10/gfx10DmaCmdBuffer.cpp#L158-L173
[pal-nop-fields]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx10/gfx10_merged_sdma_packets.h#L2522-L2545
[pal-timestamp]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/sdma/gfx10/gfx10DmaCmdBuffer.cpp#L107-L137
[rocr-overlap]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/inc/hsa_ext_amd.h#L2090-L2116
[pal-factory]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9Device.cpp#L2943-L2947
[pal-asic]: https://github.com/GPUOpen-Drivers/pal/blob/c5e800072a32f68b6ccc4422936d96167c6e0728/src/core/hw/gfxip/gfx9/gfx9Device.cpp#L5496-L5506
[radv-admission]: https://gitlab.freedesktop.org/mesa/mesa/-/blob/0ba4b08edc65075e9346d20d5310261939aaaf48/src/amd/vulkan/radv_physical_device.c#L2501-L2512
[npd-name]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/inc/amd_blit_sdma.h#L584-L590
[npd-layout]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v7_1_0_pkt_open.h#L131-L135
[npd-zero]: https://github.com/torvalds/linux/blob/50d05c7c76c96b90462f24debacca971d2e86713/drivers/gpu/drm/amd/amdgpu/sdma_v7_1.c#L1716-L1731
[npd-transport]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_gpu_agent.cpp#L852-L884
[npd-builder]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L2140-L2184
[rocr-ring]: https://github.com/ROCm/rocm-systems/blob/8d57824901ffa7d961c00a37d055a108723b93ca/projects/rocr-runtime/runtime/hsa-runtime/core/runtime/amd_blit_sdma.cpp#L1954-L2094
